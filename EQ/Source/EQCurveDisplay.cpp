#include "EQCurveDisplay.h"
#include "EQLookAndFeel.h"
#include <cmath>
#include <functional>
#include <limits>
#include <vector>

namespace
{
    // The three view presets, matching typical EQ plugin conventions
    // (Pro-Q included). Index 1 (12 dB) is the load-time default. 30 dB (not
    // 18) is the widest option — a single band caps at +/-18 dB, but the
    // COMPOSITE curve sums every active band's dB contribution at each
    // frequency (series filters cascade), so overlapping boosts can exceed
    // any one band's own limit. 30 dB gives headroom to see that.
    constexpr float kRangePresets[] = { 6.0f, 12.0f, 30.0f };
    constexpr int   kNumRangePresets = 3;

    // Dual-FFT low-end blend: a single fast-window FFT's bin spacing is too
    // coarse to resolve bass detail, so below this crossover the display
    // reads from a second, bigger-window ("slow") analyzer instance instead
    // (see EQAudioProcessor::analyzer / analyzerLow). Crossfaded smoothly
    // over kCrossfadeOctaves centred on the crossover (in log-frequency
    // space, matching the rest of this display) so there's no audible/visible
    // seam where the two analyzers' outputs meet.
    constexpr float kCrossoverHz      = 250.0f;
    constexpr float kCrossfadeOctaves = 1.0f;

    // Cosmetic display-only "slope" tilt, matching a convention most
    // reference analyzers (including Pro-Q) apply by default: real, mastered
    // music naturally carries more RAW energy in the low octaves than the
    // high ones (drums/bass/sub content concentrate a lot of energy down
    // there — this is normal, not a mix problem), so an UNTILTED analyzer
    // reads as bass-heavy for perfectly ordinary material. Reference
    // analyzers counter this by tilting the display UP toward the highs (and
    // down toward the lows) by a few dB per octave, so typical program
    // material reads roughly flat/even instead. This has ZERO effect on the
    // actual audio or the EQ's own DSP/response curve — it only reshapes
    // what the spectrum-analyzer FILL looks like. Pivoted at 1 kHz (a common
    // convention) so the curve rotates around the middle of the spectrum
    // rather than shifting the whole thing up or down.
    constexpr float kAnalyzerTiltDbPerOctave = 3.0f;
    constexpr float kAnalyzerTiltPivotHz     = 1000.0f;

    // The tilt above fades out smoothly over this many dB immediately above
    // the analyzer's floor, reaching exactly zero tilt AT the floor. Without
    // this, a bin fading down toward silence would have the FULL tilt applied
    // right up until it touches the floor, then snap to zero tilt in one
    // frame -- a visible discontinuity, worst in the highs where the tilt is
    // largest. See analyzerDb() for where this is used.
    constexpr float kTiltFadeRangeDb = 10.0f;

    // Shared by both the pre and post analyzer readings (see EQCurveDisplay::
    // analyzerDb / analyzerPostDb) so the tilt/fade behaviour is identical for
    // both curves.
    float applyAnalyzerTilt (float rawDb, float freqMid)
    {
        const float octavesFromPivot = std::log2 (freqMid / kAnalyzerTiltPivotHz);
        const float tiltAmount = kAnalyzerTiltDbPerOctave * octavesFromPivot;

        const float aboveFloor = rawDb - SpectrumAnalyzer::floorDb;
        const float fade = juce::jlimit (0.0f, 1.0f, aboveFloor / kTiltFadeRangeDb);
        return rawDb + tiltAmount * fade;
    }

    // Blends a fast/slow analyzer PAIR into one raw (un-tilted) dB reading for
    // a frequency range, crossfading smoothly around the crossover so there's
    // no seam between them. Shared by the pre pair (analyzer/analyzerLow) and
    // the post pair (analyzerPost/analyzerPostLow) -- identical treatment for
    // both, so they only diverge where the EQ itself is actually doing
    // something, not because of any difference in analysis resolution.
    float blendFastSlow (SpectrumAnalyzer& fast, SpectrumAnalyzer& slow, float freqLo, float freqHi)
    {
        // Geometric mean, not arithmetic — matches how the rest of this
        // display treats frequency (log axis).
        const float freqMid = std::sqrt (freqLo * freqHi);

        const float loEdge = kCrossoverHz * std::pow (2.0f, -kCrossfadeOctaves * 0.5f);
        const float hiEdge = kCrossoverHz * std::pow (2.0f,  kCrossfadeOctaves * 0.5f);

        if (freqMid <= loEdge) return slow.getDbForFrequencyRange (freqLo, freqHi);
        if (freqMid >= hiEdge) return fast.getDbForFrequencyRange (freqLo, freqHi);

        // Smoothstep across the crossfade band in log-frequency space.
        const float t = (std::log (freqMid) - std::log (loEdge)) / (std::log (hiEdge) - std::log (loEdge));
        const float w = t * t * (3.0f - 2.0f * t);

        // Blend in POWER (linear amplitude), same reasoning as every other
        // multi-source average in this codebase — dB values shouldn't be
        // averaged directly, since that under-weights the louder source.
        const float slowAmp = juce::Decibels::decibelsToGain (
            slow.getDbForFrequencyRange (freqLo, freqHi), SpectrumAnalyzer::floorDb);
        const float fastAmp = juce::Decibels::decibelsToGain (
            fast.getDbForFrequencyRange (freqLo, freqHi), SpectrumAnalyzer::floorDb);
        const float blendedAmp = slowAmp * (1.0f - w) + fastAmp * w;
        return juce::Decibels::gainToDecibels (blendedAmp, SpectrumAnalyzer::floorDb);
    }

    // Hover-strip geometry (see hoverStripBand()'s header comment). The
    // ACTION strip (shape, power, headphones, X) sits kActionStripLift
    // from the dot -- above it normally, below it for dots under the
    // graph's centre line (see actionStripRect).
    constexpr float kActionIconW = 14.0f, kActionIconH = 12.0f, kActionIconGap = 4.0f;
    // The strip's width follows its cell count (see stripCellCount): mute +
    // listen for every band, a slope handle prepended for pass filters, and
    // the four-knee slope picker while that handle is hovered.
    float actionStripWidthFor (int cells)
    {
        return kActionIconW * (float) cells + kActionIconGap * (float) (cells - 1);
    }
    constexpr float kActionStripLift = 20.0f;


    // Builds a monochrome Drawable from Tabler outline SVG path data, same
    // technique/licence note as PluginEditor's makeTablerIcon -- duplicated
    // here (small) rather than exported, since these three icons now live
    // on the graph. Recoloured to ink() so they follow the palette.
    std::unique_ptr<juce::Drawable> makeStripIcon (const juce::String& innerPaths)
    {
        const juce::String svg =
            "<svg xmlns=\"http://www.w3.org/2000/svg\" viewBox=\"0 0 24 24\" "
            "fill=\"none\" stroke=\"black\" stroke-width=\"1.5\" "
            "stroke-linecap=\"round\" stroke-linejoin=\"round\">" + innerPaths + "</svg>";

        if (auto xml = juce::XmlDocument::parse (svg))
        {
            auto drawable = juce::Drawable::createFromSVG (*xml);
            if (drawable != nullptr)
                drawable->replaceColour (juce::Colours::black, EQLookAndFeel::ink());
            return drawable;
        }
        return nullptr;
    }

    // The three action-strip icons (Tabler "power", "headphones", "x" --
    // exact path data as fetched from github.com/tabler/tabler-icons).
    // Created lazily on first use, on the message thread.
    const juce::Drawable* actionStripIcon (int index)
    {
        static const std::unique_ptr<juce::Drawable> icons[3]
        {
            makeStripIcon ("<path d=\"M7 6a7.75 7.75 0 1 0 10 0\" />"
                           "<path d=\"M12 4l0 8\" />"),
            makeStripIcon ("<path d=\"M4 15a2 2 0 0 1 2 -2h1a2 2 0 0 1 2 2v3a2 2 0 0 1 -2 2h-1a2 2 0 0 1 -2 -2l0 -3\" />"
                           "<path d=\"M15 15a2 2 0 0 1 2 -2h1a2 2 0 0 1 2 2v3a2 2 0 0 1 -2 2h-1a2 2 0 0 1 -2 -2l0 -3\" />"
                           "<path d=\"M4 15v-3a8 8 0 0 1 16 0v3\" />"),
            makeStripIcon ("<path d=\"M18 6l-12 12\" />"
                           "<path d=\"M6 6l12 12\" />")
        };
        return juce::isPositiveAndBelow (index, 3) ? icons[index].get() : nullptr;
    }

    // Mini response-curve glyph for a band's TYPE -- drawn as the boost
    // shape (matches the mental image the dropdown names conjured), in the
    // same thin-stroke language as the slope knees. The action strip's
    // first cell draws the band's CURRENT type with this, so every band
    // advertises its shape at a glance; clicking it opens the type menu
    // (see showTypeMenu()).
    void drawTypeGlyphImpl (juce::Graphics& g, EQBand::FilterType type, juce::Rectangle<float> r)
    {
        const float l = r.getX(), rt = r.getRight();
        const float ty = r.getY() + 1.0f, by = r.getBottom() - 1.0f;
        const float my = r.getCentreY();
        const float w = r.getWidth();
        const float cx = r.getCentreX();

        juce::Path p;
        switch (type)
        {
            case EQBand::FilterType::LowShelf:   // raised low end stepping down
                p.startNewSubPath (l, ty);
                p.cubicTo (l + w * 0.45f, ty, rt - w * 0.45f, by, rt, by);
                break;

            case EQBand::FilterType::HighShelf:  // flat rising into a raised top end
                p.startNewSubPath (l, by);
                p.cubicTo (l + w * 0.45f, by, rt - w * 0.45f, ty, rt, ty);
                break;

            case EQBand::FilterType::HighPass:   // rise from the floor, then flat
                p.startNewSubPath (l, by);
                p.lineTo (l + w * 0.45f, ty);
                p.lineTo (rt, ty);
                break;

            case EQBand::FilterType::LowPass:    // flat, then drop to the floor
                p.startNewSubPath (l, ty);
                p.lineTo (rt - w * 0.45f, ty);
                p.lineTo (rt, by);
                break;

            case EQBand::FilterType::Notch:      // flat with a V carved out
                p.startNewSubPath (l, ty);
                p.lineTo (cx - w * 0.2f, ty);
                p.lineTo (cx, by);
                p.lineTo (cx + w * 0.2f, ty);
                p.lineTo (rt, ty);
                break;

            case EQBand::FilterType::Bell:       // symmetric bump
            default:
                p.startNewSubPath (l, my);
                p.cubicTo (cx - w * 0.28f, my, cx - w * 0.3f, ty, cx, ty);
                p.cubicTo (cx + w * 0.3f, ty, cx + w * 0.28f, my, rt, my);
                break;
        }

        g.strokePath (p, juce::PathStrokeType (1.5f, juce::PathStrokeType::curved,
                                               juce::PathStrokeType::rounded));
    }

    // The steepness glyph for one slope choice: a flat run into a drop
    // whose horizontal span shrinks as the slope index rises, so 12 dB/oct
    // draws a gentle fall and 48 a near-vertical one. Mirrored for
    // High-Pass (rises from the left) vs Low-Pass (falls to the right), so
    // it always points the same way as the curve it controls.
    void drawSlopeGlyph (juce::Graphics& g, int slopeIndex, bool mirrored, juce::Rectangle<float> r)
    {
        const float dropSpan = r.getWidth() * (0.85f - 0.20f * (float) slopeIndex);

        juce::Path p;
        if (mirrored)
        {
            p.startNewSubPath (r.getRight(), r.getY());
            p.lineTo          (r.getX() + dropSpan, r.getY());
            p.lineTo          (r.getX(), r.getBottom());
        }
        else
        {
            p.startNewSubPath (r.getX(), r.getY());
            p.lineTo          (r.getRight() - dropSpan, r.getY());
            p.lineTo          (r.getRight(), r.getBottom());
        }

        g.strokePath (p, juce::PathStrokeType (1.5f, juce::PathStrokeType::curved,
                                               juce::PathStrokeType::rounded));
    }

    // Edge-detects a menu row's highlight and reports the row's name once,
    // on the way IN only.
    //
    // Called from paint() because PopupMenu::CustomComponent::setHighlighted
    // is not virtual -- there's no callback to override -- but it always
    // repaints, so paint() catches every transition. The side effect is
    // deliberately kept to "hand a string to a component elsewhere", which
    // can't re-enter this paint.
    //
    // Only the gained-highlight edge fires: moving between rows unhighlights
    // the old one and highlights the new one in an order PopupMenu doesn't
    // promise, so clearing on the lost edge could wipe the incoming row's
    // name right after it was set. Clearing is the menu's job on dismissal
    // (see showTypeMenu).
    void reportHighlight (bool& was, bool now, const juce::String& name,
                          const std::function<void (const juce::String&)>& onHighlight)
    {
        if (now == was)
            return;

        was = now;

        if (now && onHighlight != nullptr)
            onHighlight (name);
    }

    // One row of the band-type menu, drawn as the filter's SHAPE rather
    // than its name -- the same glyph the action strip's shape cell shows,
    // so picking a type is "click the shape you want" end to end. (The
    // slope submenu keeps plain text: "12 dB/oct" reads instantly, and
    // knee glyphs for slope were already tried and dropped.) A custom
    // component rather than PopupMenu's icon slot so the glyph can be
    // vector-drawn at row size and carry its own tick/chevron marks in the
    // same margins the text rows use (EQLookAndFeel::kMenuMarkZone).
    class TypeMenuItem final : public juce::PopupMenu::CustomComponent
    {
    public:
        TypeMenuItem (EQBand::FilterType t, bool ticked, bool hasSubMenu, juce::String n)
            : type (t), isTicked (ticked), showsSubMenu (hasSubMenu), name (std::move (n)) {}

        // Fired with this row's NAME the moment it becomes highlighted, so
        // the readout pill can spell out what the glyph means (see
        // showTypeMenu). Shape-only rows are unreadable until you've learned
        // them; this is the label, shown where the eye already goes.
        std::function<void (const juce::String&)> onHighlight;

        void getIdealSize (int& idealWidth, int& idealHeight) override
        {
            idealWidth  = 52 + EQLookAndFeel::kMenuMarkZone * 2;
            idealHeight = 26; // roomier than the 22px text rows -- the glyph needs it
        }

        void paint (juce::Graphics& g) override
        {
            reportHighlight (wasHighlighted, isItemHighlighted(), name, onHighlight);

            auto r = getLocalBounds().reduced (1);

            // Same highlight treatment as the text rows (ink fill, content
            // drawn in the background colour on top).
            auto contentColour = EQLookAndFeel::ink();
            if (isItemHighlighted())
            {
                g.setColour (EQLookAndFeel::ink());
                g.fillRect (r);
                contentColour = EQLookAndFeel::background();
            }

            g.setColour (contentColour);

            if (isTicked)
                g.drawText (juce::String (juce::CharPointer_UTF8 ("\xE2\x9C\x93")),
                            r.removeFromLeft (EQLookAndFeel::kMenuMarkZone),
                            juce::Justification::centred);

            if (showsSubMenu)
            {
                auto zone = getLocalBounds().reduced (1)
                                .removeFromRight (EQLookAndFeel::kMenuMarkZone).toFloat();
                const float h = 7.0f, w = h * 0.5f;
                const float cx = zone.getCentreX(), cy = zone.getCentreY();

                juce::Path chevron;
                chevron.startNewSubPath (cx - w * 0.5f, cy - h * 0.5f);
                chevron.lineTo          (cx + w * 0.5f, cy);
                chevron.lineTo          (cx - w * 0.5f, cy + h * 0.5f);
                g.strokePath (chevron, juce::PathStrokeType (1.2f, juce::PathStrokeType::curved,
                                                             juce::PathStrokeType::rounded));
            }

            // Glyph centred in the FULL row (not the space left over after
            // the marks), so every shape sits on the same axis whether or
            // not its row is ticked.
            drawTypeGlyphImpl (g, type, getLocalBounds().toFloat()
                                        .withSizeKeepingCentre (34.0f, 13.0f));
        }

    private:
        EQBand::FilterType type;
        bool isTicked, showsSubMenu;
        juce::String name;
        bool wasHighlighted = false;
    };

    // One row of a pass filter's slope submenu, drawn as a steepness knee
    // instead of "24 dB/oct" -- so the whole type menu is one visual
    // language (pick the shape, then pick how steep), matching its parent
    // rows. Same row geometry/mark margins as TypeMenuItem.
    class SlopeMenuItem final : public juce::PopupMenu::CustomComponent
    {
    public:
        SlopeMenuItem (int slopeIndexIn, bool mirroredIn, bool ticked, juce::String n)
            : slopeIndex (slopeIndexIn), mirrored (mirroredIn), isTicked (ticked),
              name (std::move (n)) {}

        std::function<void (const juce::String&)> onHighlight;  // see TypeMenuItem

        void getIdealSize (int& idealWidth, int& idealHeight) override
        {
            idealWidth  = 52 + EQLookAndFeel::kMenuMarkZone * 2;
            idealHeight = 26;
        }

        void paint (juce::Graphics& g) override
        {
            reportHighlight (wasHighlighted, isItemHighlighted(), name, onHighlight);

            auto r = getLocalBounds().reduced (1);

            auto contentColour = EQLookAndFeel::ink();
            if (isItemHighlighted())
            {
                g.setColour (EQLookAndFeel::ink());
                g.fillRect (r);
                contentColour = EQLookAndFeel::background();
            }

            g.setColour (contentColour);

            if (isTicked)
                g.drawText (juce::String (juce::CharPointer_UTF8 ("\xE2\x9C\x93")),
                            r.removeFromLeft (EQLookAndFeel::kMenuMarkZone),
                            juce::Justification::centred);

            drawSlopeGlyph (g, slopeIndex, mirrored,
                            getLocalBounds().toFloat().withSizeKeepingCentre (30.0f, 12.0f));
        }

    private:
        int  slopeIndex;
        bool mirrored, isTicked;
        juce::String name;
        bool wasHighlighted = false;
    };

    // dB gridline/label positions for a given +/- range. Tighter ranges get a
    // finer step so the grid never looks empty or overcrowded.
    std::vector<float> makeDbMarks (float range)
    {
        const float step = range <= 12.0f ? 3.0f : 10.0f;
        std::vector<float> marks;
        for (float db = -range; db <= range + 0.01f; db += step)
            marks.push_back (db);
        return marks;
    }
}

void EQCurveDisplay::drawTypeGlyph (juce::Graphics& g, EQBand::FilterType type, juce::Rectangle<float> r)
{
    drawTypeGlyphImpl (g, type, r);
}

juce::MouseCursor EQCurveDisplay::makeCrosshairCursor()
{
    // Small and thin -- a subtle crosshair, not a large/distracting cursor
    // replacement. 1px stroke matches the thin grid/axis lines elsewhere on
    // this graph, drawn fully opaque black (unlike those, which are faint)
    // so it still reads clearly as a cursor against the white background.
    constexpr int size = 17; // odd, so there's an exact centre pixel
    juce::Image img (juce::Image::ARGB, size, size, true);

    juce::Graphics g (img);
    g.setColour (EQLookAndFeel::ink());
    const float mid = size * 0.5f;
    g.drawLine (mid, 1.0f, mid, (float) size - 1.0f, 1.0f);
    g.drawLine (1.0f, mid, (float) size - 1.0f, mid, 1.0f);

    return juce::MouseCursor (img, size / 2, size / 2);
}

EQCurveDisplay::EQCurveDisplay (EQAudioProcessor& p)
    : proc (p)
{
    startTimerHz (30);

    // Scoped to this component alone via setMouseCursor() -- every other
    // control (knobs, dropdowns, bypass toggle, icons) keeps the normal OS
    // arrow cursor, since none of them are children of EQCurveDisplay.
    setMouseCursor (makeCrosshairCursor());
}

EQCurveDisplay::~EQCurveDisplay()
{
    // If the editor is torn down mid-hold (see listenHoldActive), don't
    // leave the processor stuck auditioning a band with no UI to release
    // it -- and close the sweep's freq gesture so the host isn't left with
    // a dangling beginChangeGesture.
    if (listenHoldActive)
    {
        if (listenHoldBand >= 0)
            endGesture (listenHoldBand, "freq");
        proc.setListenBand (-1);
    }

    stopTimer();
}

void EQCurveDisplay::resized() {}

void EQCurveDisplay::setSelectedBand (int band)
{
    if (selectedBand != band)
    {
        selectedBand = band;
        if (onSelectionChanged) onSelectionChanged();
        repaint();
    }
}

//==============================================================================
// --- parameter access -------------------------------------------------------
bool EQCurveDisplay::bandOn (int b) const
{
    return proc.apvts.getRawParameterValue (EQAudioProcessor::bandParamID (b, "on"))->load() > 0.5f;
}

bool EQCurveDisplay::bandMuted (int b) const
{
    return proc.apvts.getRawParameterValue (EQAudioProcessor::bandParamID (b, "mute"))->load() > 0.5f;
}

EQBand::FilterType EQCurveDisplay::bandType (int b) const
{
    return static_cast<EQBand::FilterType> (
        (int) proc.apvts.getRawParameterValue (EQAudioProcessor::bandParamID (b, "type"))->load());
}

float EQCurveDisplay::bandFreq (int b) const
{
    return proc.apvts.getRawParameterValue (EQAudioProcessor::bandParamID (b, "freq"))->load();
}

float EQCurveDisplay::bandGain (int b) const
{
    return proc.apvts.getRawParameterValue (EQAudioProcessor::bandParamID (b, "gain"))->load();
}

float EQCurveDisplay::bandQ (int b) const
{
    return proc.apvts.getRawParameterValue (EQAudioProcessor::bandParamID (b, "q"))->load();
}

bool EQCurveDisplay::bandIsPass (int b) const
{
    const auto t = bandType (b);
    return t == EQBand::FilterType::HighPass || t == EQBand::FilterType::LowPass;
}

int EQCurveDisplay::stripCellCount (int b) const
{
    // Auditioning: no cells at all -- the frequency readout above the strip
    // is all that stays (see isAuditioning). The strip rect keeps a nominal
    // single-cell width so that readout still centres over the dot.
    if (listenHoldActive)
        return 1;

    return slopeExpandedBand == b ? 4
         : bandIsPass (b)         ? 3
                                  : 2;
}

int EQCurveDisplay::slopeForPickerCell (int band, int cell) const
{
    return bandType (band) == EQBand::FilterType::HighPass ? cell : 3 - cell;
}

void EQCurveDisplay::updateSlopeExpansion (int liveStripBand)
{
    if (slopeExpandedBand >= 0)
    {
        // Stay expanded only while the cursor remains inside the (wider)
        // expanded strip of the SAME band it opened for -- leaving it, the
        // strip fading, or the band losing its pass type all collapse it.
        const bool still = slopeExpandedBand == liveStripBand
                        && bandOn (slopeExpandedBand)
                        && bandIsPass (slopeExpandedBand)
                        && actionStripRect (slopeExpandedBand)
                               .expanded (6.0f, 8.0f).contains (hoverPos);
        if (! still)
        {
            slopeExpandedBand = -1;
            repaint();
        }
        return;
    }

    // Expand when the cursor settles on the slope handle (a pass band's
    // first cell) of a strip that's actually showing.
    if (liveStripBand >= 0 && bandIsPass (liveStripBand) && hoverStripAlpha > 0.5f)
    {
        // The handle sits at the HP strip's left end, the LP strip's right
        // (see the paint mapping) -- the trigger zone follows it.
        const auto strip = actionStripRect (liveStripBand);
        const bool atLeft = bandType (liveStripBand) == EQBand::FilterType::HighPass;
        const juce::Rectangle<float> slopeCell (atLeft ? strip.getX()
                                                       : strip.getRight() - kActionIconW,
                                                strip.getY(),
                                                kActionIconW, kActionIconH);
        if (slopeCell.expanded (2.0f, 6.0f).contains (hoverPos))
        {
            slopeExpandedBand = liveStripBand;
            repaint();
        }
    }
}

int EQCurveDisplay::bandSlope (int b) const
{
    return (int) proc.apvts.getRawParameterValue (EQAudioProcessor::bandParamID (b, "slope"))->load();
}

bool EQCurveDisplay::bandDynOn (int b) const
{
    return proc.apvts.getRawParameterValue (EQAudioProcessor::bandParamID (b, "dynon"))->load() > 0.5f;
}

float EQCurveDisplay::bandDynRange (int b) const
{
    return proc.apvts.getRawParameterValue (EQAudioProcessor::bandParamID (b, "dynrange"))->load();
}

bool EQCurveDisplay::bandShowsDynamics (int b) const
{
    return bandOn (b) && ! bandMuted (b) && bandDynOn (b) && typeHasGain (bandType (b));
}

juce::Point<float> EQCurveDisplay::dynHandlePoint (int b) const
{
    const float x = freqToX (bandFreq (b));
    const float y = juce::jlimit (vMargin, (float) getHeight() - vMargin,
                                  gainToY (bandGain (b) + bandDynRange (b)));
    return { x, y };
}

int EQCurveDisplay::dynHandleAtPosition (juce::Point<float> pos) const
{
    int best = -1;
    float bestDist = 10.0f;

    for (int b = 0; b < EQAudioProcessor::numBands; ++b)
    {
        if (! bandShowsDynamics (b)) continue;

        const float d = pos.getDistanceFrom (dynHandlePoint (b));
        if (d < bestDist) { bestDist = d; best = b; }
    }

    // A static dot wins when it's the CLOSER target (<=, so an exact
    // overlap -- range dragged back to ~0 -- also goes to the dot: the
    // parameter you almost certainly want then is the static gain). But
    // only when actually closer: merely being inside some dot's 15px grab
    // radius shouldn't make a clearly-nearer ring ungrabbable when two
    // bands sit close together.
    if (best >= 0)
        if (const int dot = bandAtPosition (pos); dot >= 0)
            if (pos.getDistanceFrom (pointForBand (dot)) <= bestDist)
                return -1;

    return best;
}

bool EQCurveDisplay::typeHasGain (EQBand::FilterType t)
{
    return t == EQBand::FilterType::Bell
        || t == EQBand::FilterType::LowShelf
        || t == EQBand::FilterType::HighShelf;
}

void EQCurveDisplay::setBandParam (int b, juce::StringRef name, float value)
{
    if (auto* prm = proc.apvts.getParameter (EQAudioProcessor::bandParamID (b, name)))
        prm->setValueNotifyingHost (prm->convertTo0to1 (value)); // normalise via the param's own range
}

void EQCurveDisplay::beginGesture (int b, juce::StringRef name)
{
    // New undo transaction boundary here too, mirroring what JUCE's own
    // ParameterAttachment::beginGesture() already does automatically for
    // every attachment-driven control -- this is the manual-write
    // equivalent (drag an existing point, drag-create a new one, scroll to
    // change Q), so those gestures still each collapse into one undo step.
    proc.beginNewTransaction();

    if (auto* prm = proc.apvts.getParameter (EQAudioProcessor::bandParamID (b, name)))
        prm->beginChangeGesture();
}

void EQCurveDisplay::endGesture (int b, juce::StringRef name)
{
    if (auto* prm = proc.apvts.getParameter (EQAudioProcessor::bandParamID (b, name)))
        prm->endChangeGesture();
}

//==============================================================================
// --- axis mapping -----------------------------------------------------------
float EQCurveDisplay::freqToX (float freqHz) const
{
    const float w = (float) getWidth();
    const float prop = std::log10 (freqHz / minFreq) / std::log10 (maxFreq / minFreq);
    return juce::jlimit (0.0f, w, prop * w);
}

float EQCurveDisplay::xToFreq (float x) const
{
    const float w = (float) getWidth();
    const float prop = w > 0.0f ? x / w : 0.0f;
    return minFreq * std::pow (maxFreq / minFreq, prop);
}

float EQCurveDisplay::gainToY (float gainDb) const
{
    const float half = (float) getHeight() * 0.5f;
    return half - (gainDb / verticalRangeDb) * (half - vMargin);
}

float EQCurveDisplay::yToGain (float y) const
{
    const float half = (float) getHeight() * 0.5f;
    return (half - y) / juce::jmax (1.0f, half - vMargin) * verticalRangeDb;
}

void EQCurveDisplay::drawAxisLabel (juce::Graphics& g, const juce::String& text,
                                    juce::Rectangle<int> box, juce::Justification just) const
{
    // No backdrop — text drawn directly over whatever's underneath
    // (grid/curve/points), per explicit request.
    g.setColour (EQLookAndFeel::ink().withAlpha (0.55f));
    g.setFont (EQLookAndFeel::uiFont (10.0f));
    g.drawText (text, box, just);
}

float EQCurveDisplay::analyzerDb (float freqLo, float freqHi) const
{
    const float rawDb  = blendFastSlow (proc.getAnalyzer(), proc.getAnalyzerLow(), freqLo, freqHi);
    const float freqMid = std::sqrt (freqLo * freqHi);

    // Cosmetic slope/tilt (see kAnalyzerTiltDbPerOctave / applyAnalyzerTilt)
    // -- applied LAST, on top of the fast/slow blend, so it never interacts
    // with the crossfade math above.
    return applyAnalyzerTilt (rawDb, freqMid);
}

float EQCurveDisplay::analyzerPostDb (float freqLo, float freqHi) const
{
    // Same fast/slow blend + tilt as the pre curve, just fed from the POST-EQ
    // pair — matching resolution to the pre curve so the two only diverge
    // where the EQ is actually doing something, not because of any
    // difference in analysis quality between them.
    const float rawDb  = blendFastSlow (proc.getAnalyzerPost(), proc.getAnalyzerPostLow(), freqLo, freqHi);
    const float freqMid = std::sqrt (freqLo * freqHi);
    return applyAnalyzerTilt (rawDb, freqMid);
}

juce::Point<float> EQCurveDisplay::pointForBand (int b) const
{
    const float x = freqToX (bandFreq (b));
    const float rawGain = typeHasGain (bandType (b)) ? bandGain (b) : 0.0f;

    // A band can sit anywhere in its full +/-18 dB range regardless of the
    // current view scale; clamp its ON-SCREEN position to the visible edge
    // so the handle stays visible (and grabbable) instead of vanishing off
    // the top/bottom of the graph when zoomed in.
    const float y = juce::jlimit (vMargin, (float) getHeight() - vMargin, gainToY (rawGain));
    return { x, y };
}

int EQCurveDisplay::bandAtPosition (juce::Point<float> pos) const
{
    int   best = -1;
    float bestDist = 15.0f; // grab radius (px)

    for (int b = 0; b < EQAudioProcessor::numBands; ++b)
    {
        if (! bandOn (b)) continue;

        const float d = pos.getDistanceFrom (pointForBand (b));
        if (d < bestDist) { bestDist = d; best = b; }
    }
    return best;
}

//==============================================================================
void EQCurveDisplay::paint (juce::Graphics& g)
{

    auto bounds = getLocalBounds().toFloat();

    // Subtle vertical gradient instead of a flat fill -- slightly lifted
    // (toward ink) at the top, sinking toward black at the bottom, so the
    // graph reads as a gently lit display surface rather than a flat panel
    // (the same treatment Pro-Q's graph area has; its chrome stays flat,
    // and so does ours -- this is scoped to the graph only). Both endpoints
    // derive from currentBackgroundColour(), so the whole gradient washes
    // out correctly with the bypass fade (see setBackgroundDim()) -- only
    // the background shifts, nothing drawn on top loses contrast. The
    // darkening endpoint blends toward literal black (not a token): like
    // the listen vignette, "darker" is theme-independent.
    //
    // currentBackgroundColour() itself stays the FLAT MID reference for
    // everything that wants "the background colour" (cut ribbon, unselected
    // handle fill, the analyzer's bottom fade) -- at this subtlety the
    // mismatch against the local gradient shade isn't visible.
    {
        const auto base = currentBackgroundColour();
        juce::ColourGradient bgGradient = juce::ColourGradient::vertical (
            EQLookAndFeel::surfaceAt (base, 0.0f), 0.0f,
            EQLookAndFeel::surfaceAt (base, 1.0f), bounds.getHeight());
        g.setGradientFill (bgGradient);
        g.fillRect (bounds);
    }

    // --- grid -----------------------------------------------------------
    // Low-opacity black reads as "subtle gray" without introducing a second
    // hue — same trick used for every faint line on this screen. One
    // gridline (and one label) per marked frequency, so every label has a
    // line running down to it.
    static const std::vector<std::pair<float, juce::String>> freqMarks {
        { 20.0f,    "20"   }, { 50.0f,    "50"   }, { 100.0f,   "100"  },
        { 200.0f,   "200"  }, { 500.0f,   "500"  }, { 1000.0f,  "1k"   },
        { 2000.0f,  "2k"   }, { 5000.0f,  "5k"   }, { 10000.0f, "10k"  },
        { 20000.0f, "20k"  }
    };

    // Unlabeled intermediate lines -- the standard log-scale "2,3,4,6,7,8,9x
    // the decade" minor ticks reference analyzers (Pro-Q included) show
    // between the major marks above, which is what makes their grid read as
    // denser than ours did with only the 10 labeled majors. No labels on
    // these -- adding a line per intermediate value here would clutter the
    // axis with numbers nobody needs; the visual rhythm is the point.
    static const std::vector<float> minorFreqMarks {
        30.0f, 40.0f, 60.0f, 70.0f, 80.0f, 90.0f,
        300.0f, 400.0f, 600.0f, 700.0f, 800.0f, 900.0f,
        3000.0f, 4000.0f, 6000.0f, 7000.0f, 8000.0f, 9000.0f
    };

    // Grid runs the FULL component bounds now, edge-to-edge on every side —
    // matching Pro-Q's own layout instead of stopping short for a label
    // margin. Labels are drawn afterwards, on top (see drawAxisLabel below).
    // Vertical lines stop at vMargin top/bottom, matching exactly where the
    // outermost horizontal dB lines (+/-range) sit — gainToY() insets those
    // by vMargin too. Without this, the vertical lines ran the FULL height
    // and visibly poked out past the top/bottom "border" row by vMargin's
    // worth of pixels.
    g.setColour (EQLookAndFeel::ink().withAlpha (0.05f));
    for (float markFreq : minorFreqMarks)
    {
        const int x = juce::jlimit (0, getWidth() - 1, (int) freqToX (markFreq));
        g.drawVerticalLine (x, vMargin, (float) getHeight() - vMargin);
    }

    g.setColour (EQLookAndFeel::ink().withAlpha (0.10f));
    for (auto& mark : freqMarks)
    {
        // The 20 kHz mark lands at x == getWidth() exactly — the boundary
        // pixel column, one past the last valid one — so it was silently not
        // rendering at all (unlike 20 Hz at x == 0, a valid interior column).
        // Clamping keeps it just inside the visible area so both edges match.
        const int x = juce::jlimit (0, getWidth() - 1, (int) freqToX (mark.first));
        g.drawVerticalLine (x, vMargin, (float) getHeight() - vMargin);
    }

    // dB gridlines/labels are driven by the CURRENT vertical scale, so they
    // rescale live as the user cycles the range control.
    // The TARGET preset's marks, positioned through the ANIMATED mapping --
    // during a glide the +/-12 set slides into place from beyond the edges,
    // rather than fractional marks (a mid-glide range of 8.4 would label
    // lines at -8.4, -5.4, ...) flickering through integer text.
    const auto dbMarks = makeDbMarks (kRangePresets[rangePresetIndex]);

    g.setColour (EQLookAndFeel::ink().withAlpha (0.10f));
    for (float db : dbMarks)
        if (db != 0.0f)
            g.drawHorizontalLine ((int) gainToY (db), 0.0f, bounds.getWidth());

    // 0 dB reference line -- the ONE line on the grid drawn in the copper
    // accent (solid, not faded like the ink gridlines above), so the resting
    // state has a clear anchor. One of accent()'s three deliberate roles.
    g.setColour (EQLookAndFeel::accent());
    g.drawHorizontalLine ((int) gainToY (0.0f), 0.0f, bounds.getWidth());

    // --- spectrum analyzer (drawn UNDER the EQ curve) ----------------------
    // Sampled per pixel via the same xToFreq() the curve/grid use, so it lines
    // up with the log axis for free. Its dBFS -> Y mapping is INDEPENDENT of
    // the EQ's gain grid (signal level, not filter gain): floor at the bottom,
    // 0 dBFS at the top. Always Pre+Post -- pre is always shown; post reveals
    // itself (see the Q-derived window below) once a band is actually active.
    {
        const float h = bounds.getHeight();
        const int   w = getWidth();
        auto dbToY = [h] (float db)
        {
            const float p = juce::jlimit (0.0f, 1.0f,
                (db - SpectrumAnalyzer::floorDb) / (0.0f - SpectrumAnalyzer::floorDb));
            return h * (1.0f - p);
        };

        // Shared by both the pre and post curves. Samples dbFn at coarser
        // ANCHOR points (every `step` pixels, each averaging dbFn over the
        // full step it represents -- same "average every bin a pixel/anchor
        // actually spans" idea as before, just over a wider span) rather than
        // one point per pixel, then threads a Catmull-Rom spline through
        // those anchors (converted to cubic Beziers) instead of the previous
        // per-pixel local quadratic-through-midpoint chain. The old approach
        // re-kinked at every single pixel, which read as a faceted/blocky
        // ridge line rather than a smooth mountain-range shape; a spline
        // sampled from fewer, wider-spaced anchors sweeps THROUGH each peak
        // instead of just rounding each pixel-to-pixel joint individually.
        // `filled` distinguishes the two visually: pre keeps its original
        // light fill + faint contour (drawn first, so it sits underneath);
        // post is an unfilled, slightly bolder outline drawn on top of it —
        // the same "what came in vs. what's coming out" convention reference
        // analyzers use for a pre/post overlay.
        auto drawAnalyzerCurve = [&] (const std::function<float (float, float)>& dbFn,
                                      bool filled, float fillAlpha, float contourAlpha, float strokeWidth)
        {
            // Tighter than the first attempt (was 4px) -- wider spacing
            // smoothed over exactly the small peak-to-peak texture the user
            // wants visible; still coarser than 1px so the spline sweeps
            // through peaks rather than kinking at every pixel.
            constexpr float step = 1.5f;
            std::vector<juce::Point<float>> pts;
            pts.reserve ((size_t) (w / step) + 2);
            for (float x = 0.0f; x < (float) w; x += step)
            {
                const float freqLo = xToFreq (x - step * 0.5f);
                const float freqHi = xToFreq (x + step * 0.5f);
                pts.push_back ({ x, dbToY (dbFn (freqLo, freqHi)) });
            }
            {
                const float freqLo = xToFreq (pts.back().x);
                const float freqHi = xToFreq ((float) w);
                pts.push_back ({ (float) w, dbToY (dbFn (freqLo, freqHi)) });
            }

            juce::Path contour, fill;
            contour.startNewSubPath (pts.front());
            if (filled) { fill.startNewSubPath (0.0f, h); fill.lineTo (pts.front()); }

            const size_t n = pts.size();
            for (size_t i = 0; i + 1 < n; ++i)
            {
                const auto& p1 = pts[i];
                const auto& p2 = pts[i + 1];
                const auto& p0 = (i == 0)      ? p1 : pts[i - 1];
                const auto& p3 = (i + 2 < n)   ? pts[i + 2] : p2;

                const auto c1 = p1 + (p2 - p0) * (1.0f / 6.0f);
                const auto c2 = p2 - (p3 - p1) * (1.0f / 6.0f);

                contour.cubicTo (c1, c2, p2);
                if (filled) fill.cubicTo (c1, c2, p2);
            }

            if (filled)
            {
                fill.lineTo ((float) w, h);
                fill.closeSubPath();

                // Flat translucent fill under the curve. (An animated
                // photographic "skin" system used to draw a cycling image
                // here instead, with a drop shadow around this silhouette --
                // both removed; this plain fill is what the skin picker's
                // "off" option looked like, and is now the only look.)
                g.setColour (EQLookAndFeel::ink().withAlpha (fillAlpha));
                g.fillPath (fill);

                // Paints the actual BACKGROUND colour over the bottom 25% of
                // the fill, ramping from fully transparent at 75% down to
                // fully opaque at the bottom edge, so the fill reads as
                // trailing off into plain background near the bottom rather
                // than ending in a hard horizontal edge.
                const auto bg = currentBackgroundColour();
                juce::ColourGradient bottomFade (bg.withAlpha (0.0f), 0.0f, h * 0.75f,
                                                 bg.withAlpha (1.0f), 0.0f, h,
                                                 false);
                g.setGradientFill (bottomFade);
                g.fillPath (fill);
            }
            g.setColour (EQLookAndFeel::ink().withAlpha (contourAlpha));
            g.strokePath (contour, juce::PathStrokeType (strokeWidth));
        };

        // Pre is always shown. Post only means anything once the EQ is
        // ACTIVELY shaping the signal — with every placed band either muted
        // or nonexistent, pre and post are identical anyway (nothing is
        // really processing), so drawing a second on-top curve would just be
        // visual noise. Stays hidden until at least one band is on AND
        // unmuted, then appears the moment there's something real to compare.
        bool anyBandActive = false;
        for (int b = 0; b < EQAudioProcessor::numBands; ++b)
            if (bandOn (b) && ! bandMuted (b)) { anyBandActive = true; break; }

        const bool showPost = anyBandActive;

        drawAnalyzerCurve ([this] (float lo, float hi) { return analyzerDb (lo, hi); },
                          true, 0.13f, 0.28f, 1.0f);

        if (showPost)
        {
            // Q-derived reveal window (per active band: bandwidth in OCTAVES
            // from its own Q, the standard parametric-EQ conversion; a smooth
            // cosine/smoothstep taper centred on that band's frequency,
            // reaching exactly zero at +/- half that bandwidth). Multiple
            // bands combine via MAX, so overlapping bands blend naturally and
            // non-overlapping bands each get their own separate window.
            std::vector<float> windowAlpha ((size_t) w + 1, 0.0f);
            for (int b = 0; b < EQAudioProcessor::numBands; ++b)
            {
                if (! bandOn (b) || bandMuted (b)) continue; // muted bands aren't actually shaping anything

                const float freqB = bandFreq (b);
                const float q     = bandQ (b);
                const float halfBwOctaves = std::asinh (1.0f / (2.0f * q)) / std::log (2.0f);
                const auto  type  = bandType (b);

                for (int x = 0; x <= w; ++x)
                {
                    const float freqX = xToFreq ((float) x);
                    const float logRatio = std::log2 (freqX / freqB); // signed: negative below freqB, positive above

                    // Bell shapes a symmetric bump/dip around freqB, so its
                    // reveal window tapers on BOTH sides -- that's the
                    // original (and only correct) behaviour here. Shelves
                    // and pass filters shape everything on ONE SIDE of
                    // freqB out toward an edge of the spectrum (e.g. a High
                    // Shelf keeps boosting all the way to 20 kHz) -- using
                    // the same symmetric distance for those collapsed the
                    // reveal down to a thin sliver right at the corner
                    // frequency instead of covering the whole shaped region,
                    // which is the bug this fixes. Full reveal (distOctaves
                    // clamped to 0) on the side that's actually being
                    // shaped; only the OTHER side (back toward flat) tapers.
                    float distOctaves;
                    switch (type)
                    {
                        case EQBand::FilterType::LowShelf:
                        case EQBand::FilterType::HighPass:
                            distOctaves = juce::jmax (0.0f, logRatio);  // shapes BELOW freqB
                            break;
                        case EQBand::FilterType::HighShelf:
                        case EQBand::FilterType::LowPass:
                            distOctaves = juce::jmax (0.0f, -logRatio); // shapes ABOVE freqB
                            break;
                        default: // Bell
                            distOctaves = std::abs (logRatio);
                            break;
                    }

                    const float t = juce::jlimit (0.0f, 1.0f, distOctaves / halfBwOctaves);
                    const float a = 1.0f - t * t * (3.0f - 2.0f * t); // smoothstep taper: 1 at centre -> 0 at the edge
                    windowAlpha[(size_t) x] = juce::jmax (windowAlpha[(size_t) x], a);
                }
            }

            // Blend the PLOTTED POSITION between pre and post, using the
            // window as the blend factor -- NOT the curve's opacity. Fading
            // opacity left a faint, slightly-offset "ghost" line far from any
            // band (the true post reading is never exactly identical to pre,
            // even in an unaffected region, since they're independent live
            // measurements). Blending position instead makes the post line
            // genuinely COINCIDE with pre wherever the window is zero, so it
            // quietly forks off the shared curve right where a band starts
            // acting and merges back into it as the window fades out.
            std::vector<juce::Point<float>> pts;
            std::vector<float> preYs;
            std::vector<float> postYs;
            pts.reserve ((size_t) w + 1);
            preYs.reserve ((size_t) w + 1);
            postYs.reserve ((size_t) w + 1);
            for (int x = 0; x <= w; ++x)
            {
                const float freqLo = xToFreq ((float) x - 0.5f);
                const float freqHi = xToFreq ((float) x + 0.5f);
                const float preY  = dbToY (analyzerDb (freqLo, freqHi));
                const float postY = dbToY (analyzerPostDb (freqLo, freqHi));
                const float y = preY + (postY - preY) * windowAlpha[(size_t) x];
                pts.push_back ({ (float) x, y });
                preYs.push_back (preY);
                postYs.push_back (postY);
            }

            // Fill the RIBBON between the pre curve and the (blended) post
            // curve -- a tint shows exactly where the two have diverged,
            // rather than relying solely on the outline. Since `pts` already
            // coincides with `preYs` wherever windowAlpha is zero (the
            // position-blend above), each per-pixel quad naturally collapses
            // to zero width outside a band's reveal window with no extra
            // masking needed.
            //
            // Boost (post sits ABOVE pre, smaller pixel Y -- signal being
            // ADDED by the EQ) is a solid fill in the copper accent (one of
            // accent()'s three deliberate roles) -- the colour literally
            // marks "signal the EQ is adding."
            // Cut (post sits BELOW pre) stays a near-background overlay --
            // toward background, not away from it -- so a cut still reads as
            // the signal being "erased" toward background. Uses
            // currentBackgroundColour() (NOT a hardcoded colour) so it still
            // reads as "erased" after the background's own bypass fade.
            // Colour is still decided per pixel column by the LOCAL sign of
            // (post - pre), since a single band boosting can still have a
            // cutting neighbour right next to it -- but contiguous
            // same-colour columns are batched into ONE filled path per run,
            // not one independent quad per column. Independent translucent
            // quads left a lighter AA hairline at every shared column edge
            // once the 1.2x UI scale (see setScaleFactor) put those edges on
            // fractional physical pixels -- visible as vertical stripes
            // through the fill. Inside a single path the rasterizer
            // accumulates coverage across the whole run, so the interior
            // seams vanish (and ~one fillPath per run beats ~one per pixel).
            // Blended toward dimmedBackground() by backgroundDim so it
            // washes out in lockstep with the rest of the UI while bypassed,
            // instead of staying fully saturated while everything around it
            // fades.
            const auto boostColour = EQLookAndFeel::accent().interpolatedWith (EQLookAndFeel::dimmedBackground(), backgroundDim * 0.6f)
                                                            .withAlpha (0.75f);
            const auto cutColour   = currentBackgroundColour().withAlpha (0.75f);

            juce::Path runPath;        // the ribbon run being accumulated
            int        runStart = -1;  // first column index of that run, -1 = none open
            bool       runIsBoost = false;

            auto flushRun = [&] (int runEnd) // runEnd = one-past-last column index
            {
                if (runStart < 0)
                    return;

                // Top edge: pre curve, left to right. Bottom edge: the
                // blended post positions, right to left, closing the loop.
                runPath.startNewSubPath ((float) runStart, preYs[(size_t) runStart]);
                for (int x = runStart + 1; x <= runEnd; ++x)
                    runPath.lineTo ((float) x, preYs[(size_t) x]);

                for (int x = runEnd; x >= runStart; --x)
                    runPath.lineTo (pts[(size_t) x].x, pts[(size_t) x].y);
                runPath.closeSubPath();

                g.setColour (runIsBoost ? boostColour : cutColour);
                g.fillPath (runPath);
                runPath.clear();
                runStart = -1;
            };

            for (size_t i = 0; i + 1 < pts.size(); ++i)
            {
                const float d0 = pts[i].y     - preYs[i];
                const float d1 = pts[i + 1].y - preYs[i + 1];

                if (std::abs (d0) < 0.05f && std::abs (d1) < 0.05f)
                {
                    flushRun ((int) i); // ribbon has ~zero width here -- end any open run
                    continue;
                }

                const bool isBoost = (d0 + d1) < 0.0f; // post above pre on screen -> boost
                if (runStart >= 0 && isBoost != runIsBoost)
                    flushRun ((int) i); // colour flips -- close this run, start the next

                if (runStart < 0)
                {
                    runStart   = (int) i;
                    runIsBoost = isBoost;
                }
            }
            flushRun ((int) pts.size() - 1);

            // Draw per-segment (straight lines between adjacent pixels,
            // rather than one continuous smooth path) so both POSITION and
            // COLOUR can fade together as a segment leaves/enters a band's
            // window: position already blends toward pre (so it forks off/
            // rejoins spatially), and now the stroke's own alpha scales
            // continuously by the same windowAlpha too, instead of being a
            // constant 0.45 gated by an on/off threshold -- so the line
            // visibly fades IN colour as it forks away from pre, and fades
            // back OUT as it rejoins, rather than snapping to full opacity
            // right at the window's edge. Near-zero segments are still
            // skipped outright (nothing meaningful to draw, and avoids
            // stacking a second very-faint stroke exactly on pre's own line).
            //
            // The stroke tops out at EXACTLY preContourAlpha (pre's own
            // contour weight, 0.28 -- see the showPre call above) and fades
            // to zero from there. It used to sit at 0.28 as a FLOOR and ramp
            // UP to 0.45, which made the analyzer's edge visibly brighter
            // wherever a band acted -- and where the two lines run close but
            // not coincident, both get drawn and their translucency stacks,
            // brightening it further still. Capping at pre's own alpha means
            // the outline holds one weight and one brightness across the
            // whole spectrum; a band's presence shows as the line FORKING,
            // never as it getting louder.
            //
            // Fading to zero (rather than to 0.28) is safe here precisely
            // because of the realDiff gate below: by the time alpha is near
            // zero the post line is within a pixel of pre anyway, so pre's
            // own stroke is the line you see. No dip.
            constexpr float preContourAlpha = 0.28f;
            for (size_t i = 0; i + 1 < pts.size(); ++i)
            {
                const float segAlpha = 0.5f * (windowAlpha[i] + windowAlpha[i + 1]);
                if (segAlpha <= 0.02f)
                    continue;

                // How much pre and post ACTUALLY differ here, in pixels --
                // gated separately from the Q window above. With no audio
                // playing (or any other moment pre and post read identically,
                // e.g. every band muted/bypassed), postYs coincides with
                // preYs even though a band is "active," so this second
                // stroke would otherwise land exactly on top of pre's own
                // line and read as a stacked-alpha "shadow" with no real
                // signal difference behind it. Fully saturated by ~2px of
                // real divergence; skipped outright below that, rather than
                // fading only down to preContourAlpha, since drawing a
                // matching-weight stroke in the same spot as pre's still
                // visibly darkens it (semi-transparent strokes don't cancel).
                const float diffPx = 0.5f * (std::abs (postYs[i] - preYs[i]) + std::abs (postYs[i + 1] - preYs[i + 1]));
                const float realDiff = juce::jlimit (0.0f, 1.0f, diffPx / 2.0f);
                if (realDiff <= 0.02f)
                    continue;

                // Dissolve the stroke as it approaches the graph's floor --
                // where an HP/LP has filtered the signal to nothing, post
                // pins to the analyzer floor (the bottom pixel row) and this
                // stroke otherwise ran along the bottom edge as a bright
                // flat line saying nothing but "empty" ("i dont like them").
                // Full strength above ~10px from the floor, gone by ~2px --
                // the dive DOWN toward the floor stays visible (that part is
                // informative), only the landing strip disappears.
                const float segY      = 0.5f * (pts[i].y + pts[i + 1].y);
                const float floorFade = juce::jlimit (0.0f, 1.0f, (h - 2.0f - segY) / 8.0f);
                if (floorFade <= 0.02f)
                    continue;

                // SAME WIDTH as the pre contour (1.0px -- see the showPre
                // call above), not the 1.5px "slightly bolder" this used to
                // be. The mismatch was visible as the analyzer's outline
                // literally getting thicker wherever a band existed, with
                // the extra quarter-pixel down each side rendering as a
                // partially-covered lighter fringe -- the "halo". Position
                // (the line forks away from pre) and alpha (up to 0.45 vs
                // pre's 0.28) already distinguish this line; width doing it
                // too only made the seam visible.
                const float alpha = preContourAlpha * segAlpha * realDiff * floorFade;
                g.setColour (EQLookAndFeel::ink().withAlpha (alpha));
                g.drawLine (pts[i].x, pts[i].y, pts[i + 1].x, pts[i + 1].y, 1.0f);
            }
        }
    }

    // --- combined response curve -------------------------------------------
    const double sr = proc.getSampleRate() > 0.0 ? proc.getSampleRate() : 44100.0;

    // Build each on-and-unmuted band's per-stage coefficients ONCE, then
    // sample magnitude per pixel. A muted band's point still exists (see the
    // handles loop below, keyed on bandOn() alone) but it isn't really
    // shaping the signal, so it's excluded here too — this curve is supposed
    // to show the REAL response, matching what the DSP actually does. Each
    // stage now gets its OWN coefficients via EQBand::stageQFor() (Low-Pass/
    // High-Pass cascades use a different Q per stage -- the Butterworth
    // table -- not identical copies), matching EQBand::updateCoefficients()
    // exactly, so the composite curve stays a true preview of the audio.
    struct BandCoefs { std::vector<juce::dsp::IIR::Coefficients<float>::Ptr> stages; };
    std::vector<BandCoefs> coefs;
    for (int b = 0; b < EQAudioProcessor::numBands; ++b)
        if (bandOn (b) && ! bandMuted (b))
        {
            const auto type = bandType (b);
            const int stageCount = EQBand::stageCountFor (type, bandSlope (b));

            // Dynamic bands contribute their LIVE gain (static + whatever
            // the detector is currently adding) -- this is what makes the
            // main curve move with the music, mirroring exactly what the
            // audio thread hands EQBand each block. Zero for everything
            // else, so static bands are untouched.
            const float liveGain = bandGain (b) + proc.getDynamicGainDb (b);

            BandCoefs bc;
            for (int i = 0; i < stageCount; ++i)
                bc.stages.push_back (EQBand::makeCoefficients (sr, type, bandFreq (b), liveGain,
                                                                EQBand::stageQFor (type, i, stageCount, bandQ (b))));
            coefs.push_back (std::move (bc));
        }

    // The curve now runs the full component width, reaching true 20 kHz at
    // the true right edge — same edge-to-edge treatment as the grid.
    juce::Path curve;
    const int w = getWidth();
    for (int x = 0; x <= w; ++x)
    {
        const float freq = xToFreq ((float) x);

        // Series filters MULTIPLY in linear magnitude, which is the same as
        // ADDING in decibels — so we sum every stage's own dB contribution.
        double totalDb = 0.0;
        for (auto& bc : coefs)
            for (auto& c : bc.stages)
                totalDb += juce::Decibels::gainToDecibels (c->getMagnitudeForFrequency (freq, sr));

        // Clamp to the visible range so an extreme boost/cut flattens along
        // the top/bottom edge instead of running off-screen and vanishing.
        const float y = juce::jlimit (vMargin, (float) getHeight() - vMargin, gainToY ((float) totalDb));
        if (x == 0) curve.startNewSubPath ((float) x, y);
        else        curve.lineTo ((float) x, y);
    }

    // Slightly under full alpha -- at 2px this is the widest bright element
    // on screen, and at 1.0 it glared against the dark ground while every
    // other full-ink element (small text, thin rings) read fine.
    //
    // Stroked through a vertical gradient (not a flat colour) so the curve
    // DISSOLVES as it approaches the bottom edge -- same treatment as the
    // post-analyzer stroke: when a steep HP/LP's response drops below the
    // visible dB range, the curve clamps to the bottom row and otherwise
    // ran along it as a bright line underlining the Hz labels. Full
    // strength until ~10px above the edge, gone by ~2px -- the dive out of
    // view stays visible, the flat clamped run disappears. Mid-graph the
    // gradient is a constant 0.90, so normal curves are unaffected.
    {
        const float ch = (float) getHeight();
        juce::ColourGradient curveFade (EQLookAndFeel::ink().withAlpha (0.90f), 0.0f, 0.0f,
                                        EQLookAndFeel::ink().withAlpha (0.0f),  0.0f, ch - 2.0f, false);
        curveFade.addColour (juce::jlimit (0.0, 1.0, (double) ((ch - 10.0f) / (ch - 2.0f))),
                             EQLookAndFeel::ink().withAlpha (0.90f));
        g.setGradientFill (curveFade);
        g.strokePath (curve, juce::PathStrokeType (2.0f));
    }

    // --- band handles ---------------------------------------------------
    // Selected = solid black fill. Unselected = white-filled, black-outlined
    // ring, so it still reads clearly against the white background instead
    // of vanishing into it.
    for (int b = 0; b < EQAudioProcessor::numBands; ++b)
    {
        if (! bandOn (b)) continue;

        const auto pt  = pointForBand (b);
        const bool sel = (b == selectedBand);

        // Muted: the point still floats at its own freq/gain, but it isn't
        // actually shaping the curve any more (see `coefs` above, which
        // already excludes muted bands). Rather than a single vertical
        // tether, draw a dashed preview of THIS BAND'S OWN response shape
        // (its own bell/shelf/etc curve, evaluated alone, not summed with
        // the others) across the whole width -- shows exactly what shape is
        // being suppressed, not just where its point happens to sit.
        // This band's OWN response (evaluated alone, not summed with the
        // others) at an arbitrary gain, as a path -- shared by the muted
        // ghost and the dynamic-destination ghost below, so the two "faint
        // preview of one band" drawings can't drift apart in shape.
        auto singleBandPath = [&] (int band, float gainDb)
        {
            const auto bt = bandType (band);
            const int stageCount = EQBand::stageCountFor (bt, bandSlope (band));
            std::vector<juce::dsp::IIR::Coefficients<float>::Ptr> cs;
            for (int i = 0; i < stageCount; ++i)
                cs.push_back (EQBand::makeCoefficients (sr, bt, bandFreq (band), gainDb,
                                                        EQBand::stageQFor (bt, i, stageCount, bandQ (band))));

            juce::Path path;
            for (int x = 0; x <= w; ++x)
            {
                const float freq = xToFreq ((float) x);
                double db = 0.0;
                for (auto& c : cs)
                    db += juce::Decibels::gainToDecibels (c->getMagnitudeForFrequency (freq, sr));
                const float y = juce::jlimit (vMargin, (float) getHeight() - vMargin, gainToY ((float) db));
                if (x == 0) path.startNewSubPath ((float) x, y);
                else        path.lineTo ((float) x, y);
            }
            return path;
        };

        if (bandMuted (b))
        {
            auto bandPath = singleBandPath (b, bandGain (b));

            // A faint SOLID hairline, not a dashed one. Dashes were tried
            // first and read as a blueprint annotation rather than part of
            // the instrument -- they were also the only dashed thing in the
            // whole plugin, whereas "inactive" is expressed as DIMMER
            // everywhere else (muted power glyph, disabled buttons,
            // unselected slope glyphs, non-current menu rows). At 0.35
            // alpha they also competed with the real response curve while
            // representing something that isn't processing audio at all.
            g.setColour (EQLookAndFeel::ink().withAlpha (0.16f));
            g.strokePath (bandPath, juce::PathStrokeType (1.0f));
        }

        // Dynamic band: a faint hairline of where this band lands at FULL
        // dynamic action (static gain + range) -- the destination the live
        // curve breathes toward -- with a hollow ring handle on it at the
        // band's frequency. Dragging that ring sets the range; the ring
        // stays at the DESTINATION (a parameter, still and grabbable) while
        // the main curve does the moving, so the thing you aim never runs
        // away from the cursor.
        if (bandShowsDynamics (b) && std::abs (bandDynRange (b)) > 0.05f)
        {
            g.setColour (EQLookAndFeel::ink().withAlpha (0.16f));
            g.strokePath (singleBandPath (b, bandGain (b) + bandDynRange (b)),
                          juce::PathStrokeType (1.0f));

            const auto hp = dynHandlePoint (b);

            // Same magnetic swell language as the main dots, smaller.
            float hr = 4.5f;
            if (hoverPos.x >= 0.0f)
            {
                const float dist  = hoverPos.getDistanceFrom (hp);
                const float swell = 1.0f - juce::jlimit (0.0f, 1.0f, (dist - 10.0f) / 19.0f);
                hr += 1.5f * swell;
            }

            // A small solid up/down ARROW PAIR (Pro-Q's furniture for the
            // same handle), not a second circle -- two dots stacked on one
            // frequency read as two grabbable bands, while the paired
            // arrows read as "grab here, drag either way". Direction-
            // agnostic on purpose: one flipping arrow was tried first, and
            // the pair both signals the up-AND-down drag and stops the
            // marker changing shape as the range crosses zero. Solid, so
            // the ghost hairline passing underneath never cuts through it.
            const float aw  = hr * 0.85f;         // half-width of each head
            const float ah  = hr * 0.60f;         // height of each head
            const float gap = 1.5f;               // breathing room at the centre
            juce::Path arrows;
            arrows.addTriangle (hp.x - aw, hp.y - gap,
                                hp.x + aw, hp.y - gap,
                                hp.x,      hp.y - gap - ah); // upper head, pointing up
            arrows.addTriangle (hp.x - aw, hp.y + gap,
                                hp.x + aw, hp.y + gap,
                                hp.x,      hp.y + gap + ah); // lower head, pointing down
            g.setColour (EQLookAndFeel::ink().withAlpha (
                b == selectedBand ? 0.9f : 0.6f));
            g.fillPath (arrows);
        }

        // Magnetic proximity swell: the dot grows as the cursor approaches,
        // signalling "you can grab me from here." Held at FULL swell inside
        // 15px -- exactly bandAtPosition()'s real grab radius, so maximum
        // swell literally means a click lands on this dot -- then tapering
        // smoothly back to resting size by ~34px out. The 30Hz repaint
        // keeps it continuous as the cursor moves; a dot being dragged is
        // at distance ~0, so it stays fully swollen for free.
        float r = 6.0f;
        if (hoverPos.x >= 0.0f)
        {
            const float dist  = hoverPos.getDistanceFrom (pt);
            const float swell = 1.0f - juce::jlimit (0.0f, 1.0f, (dist - 15.0f) / 19.0f);
            r += 2.5f * swell;
        }

        if (sel)
        {
            // Selected handle fills in the copper accent (one of accent()'s
            // three deliberate roles) -- unselected handles stay neutral
            // ink rings, so the accent marks exactly one thing: the band
            // you're working on.
            g.setColour (EQLookAndFeel::accent());
            g.fillEllipse (pt.x - r, pt.y - r, r * 2.0f, r * 2.0f);
        }
        else
        {
            // White fill here is meant to "erase to background", not stand
            // out -- use the actual current background colour (matches the
            // dimmed grey while bypassed) instead of a hardcoded white, or
            // unselected points read as stark white patches once dimmed.
            g.setColour (currentBackgroundColour());
            g.fillEllipse (pt.x - r, pt.y - r, r * 2.0f, r * 2.0f);
            g.setColour (EQLookAndFeel::ink());
            g.drawEllipse (pt.x - r, pt.y - r, r * 2.0f, r * 2.0f, 1.5f);
        }
    }

    // --- listen bandwidth markers -------------------------------------------
    // While a band is being auditioned (see EQAudioProcessor::getListenBand),
    // show two vertical lines marking the actual bandpass slice being heard
    // -- same Q-to-octave-bandwidth formula already used for the post-
    // analyzer's Q-derived reveal window, so this is genuinely the frequency
    // range the listen filter is using, not a separate approximation.
    {
        const int listenBand = proc.getListenBand();
        if (listenBand >= 0 && listenBand < EQAudioProcessor::numBands && bandOn (listenBand))
        {
            const float freq = bandFreq (listenBand);
            // Drawn from the GLIDED Q (see listenWallQ / timerCallback),
            // not the raw parameter -- the raw value steps a full wheel
            // notch at a time and made the walls jump in ticks; freq stays
            // raw since dragging already delivers it continuously.
            const float q    = (listenWallBand == listenBand && listenWallQ > 0.0f)
                                   ? listenWallQ : bandQ (listenBand);
            const float halfBwOctaves = std::asinh (1.0f / (2.0f * q)) / std::log (2.0f);
            const float loFreq = freq / std::pow (2.0f, halfBwOctaves);
            const float hiFreq = freq * std::pow (2.0f, halfBwOctaves);
            const float loX = freqToX (loFreq);
            const float hiX = freqToX (hiFreq);

            // Spotlight/vignette shadow OUTSIDE the audible slice (Pro-Q's
            // effect): dims everything already drawn there -- grid, analyzer
            // fill, curve, points -- in one pass, rather than needing to
            // touch how any of those individually render. Drawn BEFORE the
            // dashed edge lines so the lines still read crisply on top.
            // Deliberately LITERAL black, not ink() -- this is a dimming
            // wash, dark by definition on any theme; ink() here would have
            // painted a BRIGHTENING off-white wash on the dark theme,
            // inverting the effect's whole meaning.
            g.setColour (juce::Colours::black.withAlpha (0.18f));
            g.fillRect (juce::Rectangle<float> (0.0f, 0.0f, loX, (float) getHeight()));
            g.fillRect (juce::Rectangle<float> (hiX, 0.0f, (float) getWidth() - hiX, (float) getHeight()));

            // Extra drop-shadow gradient hugging the OUTSIDE of each edge,
            // darkest right at the line and fading out over a short distance
            // -- reads as a shadow being CAST by the elevated spotlight
            // section onto the dimmed area next to it, not just a flat dim.
            // Literal black for the same reason as the vignette above.
            constexpr float shadowWidth = 14.0f;
            {
                const float x0 = juce::jmax (0.0f, loX - shadowWidth);
                juce::ColourGradient grad (juce::Colours::black.withAlpha (0.30f), loX, 0.0f,
                                          juce::Colours::black.withAlpha (0.0f), x0, 0.0f, false);
                g.setGradientFill (grad);
                g.fillRect (juce::Rectangle<float> (x0, 0.0f, loX - x0, (float) getHeight()));
            }
            {
                const float x1 = juce::jmin ((float) getWidth(), hiX + shadowWidth);
                juce::ColourGradient grad (juce::Colours::black.withAlpha (0.30f), hiX, 0.0f,
                                          juce::Colours::black.withAlpha (0.0f), x1, 0.0f, false);
                g.setGradientFill (grad);
                g.fillRect (juce::Rectangle<float> (hiX, 0.0f, x1 - hiX, (float) getHeight()));
            }

            g.setColour (EQLookAndFeel::ink().withAlpha (0.3f));
            g.drawLine (loX, vMargin, loX, (float) getHeight() - vMargin, 1.0f);
            g.drawLine (hiX, vMargin, hiX, (float) getHeight() - vMargin, 1.0f);
        }
    }

    // --- hover strips (actions + slope) -----------------------------------
    // The band's controls, floating above its dot (see hoverStripBand() in
    // the header for the full design): the action strip (power/listen/
    // delete) for every band, plus the 12/24/36/48 slope selector stacked
    // above it for HP/LP. Drawn after the listen vignette so neither gets
    // dimmed.
    {
        // Drawn from the LATCHED band + smoothed alpha (see timerCallback's
        // fade block), not hoverStripBand() directly -- so everything eases
        // in on approach and eases back out at its last position after the
        // cursor leaves, instead of popping ("it feels like a jump").
        const int sb = hoverStripShownBand;
        if (sb >= 0 && bandOn (sb) && hoverStripAlpha > 0.02f)
        {
            // Action strip: the band's SHAPE (its current type as a mini
            // response glyph -- click opens the type menu), power (dims
            // when the band is muted, mirroring the old row-1 power
            // toggle's convention), headphones (dims while this band is
            // being auditioned), X (delete). Each cell lifts brighter
            // under direct hover.
            {
                const auto strip = actionStripRect (sb);
                const bool muted     = bandMuted (sb);
                const bool listening = (proc.getListenBand() == sb);

                // This band's own frequency, repeated here so it can be read
                // without looking down at the axis row -- which matters most
                // while dragging, when your eye is on the dot. Same plain-
                // integer format and 10px type as the axis-row readout.
                // Sits on the strip's OUTWARD side (above when the strip is
                // above the dot, below when it's flipped underneath), so it
                // never gets squeezed into the gap between dot and icons.
                {
                    constexpr float freqH = 17.0f;
                    const bool stripAbove = strip.getCentreY() < pointForBand (sb).y;

                    // Clamped inside the graph: a band sitting high enough
                    // pins its strip near the top edge, and this row would
                    // otherwise be drawn off-screen above it.
                    const float freqY = juce::jlimit (
                        2.0f, (float) getHeight() - freqH - 2.0f,
                        stripAbove ? strip.getY() - freqH - 3.0f : strip.getBottom() + 3.0f);

                    const juce::Rectangle<float> freqRow (strip.getX() - 16.0f, freqY,
                                                          strip.getWidth() + 32.0f, freqH);

                    g.setColour (EQLookAndFeel::ink().withAlpha (0.85f * hoverStripAlpha));
                    g.setFont (EQLookAndFeel::uiFont (14.0f));
                    g.drawText (juce::String ((int) std::lround (bandFreq (sb))),
                                freqRow, juce::Justification::centred);
                }

                const int  cells    = stripCellCount (sb);
                const bool expanded = (slopeExpandedBand == sb);
                const bool hideCells = listenHoldActive; // frequency only
                const bool isPass   = bandIsPass (sb);
                const bool mirrored = bandType (sb) == EQBand::FilterType::HighPass;

                for (int i = 0; i < cells && ! hideCells; ++i)
                {
                    const juce::Rectangle<float> cell (strip.getX() + (float) i * (kActionIconW + kActionIconGap),
                                                       strip.getY(), kActionIconW, kActionIconH);

                    const bool hotCell = cell.expanded (2.0f, 6.0f).contains (hoverPos);

                    if (expanded)
                    {
                        // The four slope choices, replacing mute/listen while
                        // the handle is hovered. Current slope stays fully
                        // lit so the picker doubles as the readout.
                        const int slopeAtCell = slopeForPickerCell (sb, i);

                        float a = slopeAtCell == bandSlope (sb) ? 1.0f : 0.55f;
                        if (! hotCell)
                            a *= 0.8f;

                        g.setColour (EQLookAndFeel::ink().withAlpha (a * hoverStripAlpha));
                        drawSlopeGlyph (g, slopeAtCell, mirrored, cell.reduced (1.0f));
                        continue;
                    }

                    // Collapsed: for pass bands the slope handle sits on
                    // the side the filter FALLS toward -- first cell for a
                    // High-Pass, last for a Low-Pass ([slope mute listen]
                    // vs [listen mute slope]) -- so the mark and its filter
                    // point the same way. Mute holds the middle either way;
                    // non-pass bands stay [mute listen]. The handle is a
                    // FIXED corner mark rather than the band's current
                    // knee: a changing handle read as a fourth mystery
                    // knee, while a constant mark reads as "the slope
                    // control lives here" (the expanded picker's lit cell
                    // is the readout).
                    const int slopeCell = ! isPass ? -1 : mirrored ? 0 : cells - 1;

                    if (i == slopeCell)
                    {
                        const float a = hotCell ? 1.0f : 0.75f;
                        g.setColour (EQLookAndFeel::ink().withAlpha (a * hoverStripAlpha));

                        const auto gr = cell.reduced (2.0f, 1.5f);
                        juce::Path handle;
                        if (mirrored)
                        {
                            handle.startNewSubPath (gr.getRight(), gr.getY());
                            handle.lineTo          (gr.getX() + gr.getWidth() * 0.55f, gr.getY());
                            handle.lineTo          (gr.getX(), gr.getBottom());
                        }
                        else
                        {
                            handle.startNewSubPath (gr.getX(), gr.getY());
                            handle.lineTo          (gr.getRight() - gr.getWidth() * 0.55f, gr.getY());
                            handle.lineTo          (gr.getRight(), gr.getBottom());
                        }
                        g.strokePath (handle, juce::PathStrokeType (1.5f, juce::PathStrokeType::curved,
                                                                    juce::PathStrokeType::rounded));
                        continue;
                    }

                    // 0 = power, 1 = headphones. Pass bands: mute in the
                    // middle, listen at whichever end the slope isn't.
                    const int iconIdx = ! isPass ? i
                                      : i == 1   ? 0
                                                 : 1;

                    float base = 1.0f;
                    if (iconIdx == 0 && muted)     base = 0.45f;
                    if (iconIdx == 1 && listening) base = 0.45f;
                    if (! hotCell)
                        base *= 0.75f;

                    if (auto* icon = actionStripIcon (iconIdx))
                        icon->drawWithin (g, cell.reduced (1.0f),
                                          juce::RectanglePlacement::centred,
                                          base * hoverStripAlpha);
                }
            }
        }
    }

    // --- axis labels ------------------------------------------------------
    // Drawn LAST, on top of the grid/curve/points/analyzer (bare text, no
    // backdrop — a backdrop was tried and removed at the user's request).
    for (auto& mark : freqMarks)
    {
        const int labelX = juce::jlimit (0, getWidth() - 28, (int) freqToX (mark.first) - 14);
        drawAxisLabel (g, mark.second, { labelX, getHeight() - 10, 28, 10 },
                      juce::Justification::centred);
    }

    for (float db : dbMarks)
    {
        // Skip the very bottom-most mark — it always lands in the same
        // bottom-right corner as the "20k" Hz label, and the two collide.
        // Dropping this one number makes room rather than fighting over it.
        if (db <= -kRangePresets[rangePresetIndex] + 0.01f)
            continue;

        const juce::String label = (db > 0.0f ? "+" : "") + juce::String ((int) db);
        const int labelY = juce::jlimit (0, getHeight() - 12, (int) gainToY (db) - 6);
        drawAxisLabel (g, label, { getWidth() - 30, labelY, 26, 12 },
                      juce::Justification::centredRight);
    }

    // --- dB range bubble ---------------------------------------------------
    // The vertical-scale selector, ON the graph (top-left) instead of a
    // ComboBox in the bottom section -- click pops the 6/12/30 dB menu (see
    // showRangeMenu(), intercepted first in mouseDown()). Same magnetic
    // proximity swell as the band handles: grows as the cursor approaches,
    // held at full anywhere over the pill itself. Filled with the
    // background gradient's own shade at this row so it reads as a bubble
    // sitting on the surface, not a patch of the wrong tone.
    {
        const auto resting = rangeBubbleBounds();

        float swell = 0.0f;
        if (hoverPos.x >= 0.0f)
        {
            const float dist = hoverPos.getDistanceFrom (resting.getCentre());
            swell = 1.0f - juce::jlimit (0.0f, 1.0f, (dist - 26.0f) / 19.0f);
        }

        // Fixed 1.2px-per-side swell and a 0.40 -> 0.65 border ramp --
        // matched to the make-dynamic chip and the trim pill, so all three
        // pill controls share one physics. (This one predates them with a
        // proportional 12% grow and a hotter border; retuned to match.)
        const auto pill = resting.expanded (1.2f * swell);

        g.setColour (EQLookAndFeel::surfaceAt (currentBackgroundColour(),
                                               pill.getCentreY() / bounds.getHeight()));
        g.fillRoundedRectangle (pill, pill.getHeight() * 0.5f);

        g.setColour (EQLookAndFeel::ink().withAlpha (0.40f + 0.25f * swell));
        g.drawRoundedRectangle (pill, pill.getHeight() * 0.5f, 1.0f);

        g.setColour (EQLookAndFeel::ink().withAlpha (0.85f));
        g.setFont (EQLookAndFeel::uiFont (11.0f));
        g.drawText (juce::String ((int) kRangePresets[rangePresetIndex]) + " dB",
                    pill, juce::Justification::centred);
    }

}

//==============================================================================
// A band is "placed" (has a point) exactly when its "on" parameter is true.
// An unused band is one whose "on" is false — fully inactive in the DSP.
int EQCurveDisplay::findUnusedBandNearest (float freqHz) const
{
    int   best = -1;
    float bestDist = std::numeric_limits<float>::max();

    for (int b = 0; b < EQAudioProcessor::numBands; ++b)
    {
        if (bandOn (b))
            continue; // already placed

        // Nearest in LOG frequency, matching the on-screen X axis. This keeps
        // the pop-in tiny: the freed band we reuse is already near the click,
        // so its smoothed frequency barely has to glide. (No DSP change — just
        // a smarter pick of which spare band to wake up.)
        const float d = std::abs (std::log (bandFreq (b)) - std::log (freqHz));
        if (d < bestDist) { bestDist = d; best = b; }
    }

    return best; // -1 => all six bands are already placed
}

void EQCurveDisplay::createBandAt (int band, juce::Point<float> pos)
{
    const float freq = juce::jlimit (minFreq, maxFreq, xToFreq (pos.x));
    const float gain = juce::jlimit (-18.0f, 18.0f, yToGain (pos.y));

    // Clicking out at the extreme ends of the spectrum almost always means
    // "cut everything below/above this" rather than "bell-shape this edge",
    // so default to a High-Pass below 50 Hz / Low-Pass above 10 kHz instead
    // of a Bell there. Anywhere in between still defaults to Bell as before.
    const auto defaultType = freq < 50.0f  ? EQBand::FilterType::HighPass
                            : freq > 10000.0f ? EQBand::FilterType::LowPass
                                              : EQBand::FilterType::Bell;

    // Configure the band's shape while it is still OFF, then switch it on LAST,
    // so the audio thread never sees the band active with stale values.
    setBandParam (band, "type", (float) (int) defaultType);
    setBandParam (band, "q",    1.0f);                                   // default Q
    setBandParam (band, "freq", freq);
    setBandParam (band, "gain", gain);
    setBandParam (band, "mute", 0.0f);   // a fresh band is never muted
    setBandParam (band, "dynon",    0.0f); // and starts static --
    setBandParam (band, "dynrange", 0.0f); // no leftover dynamics from a past life
    setBandParam (band, "on",   1.0f);   // activate — now the DSP processes it

    // Adding a band should kick you out of listen/audition mode entirely
    // (on whichever band it was engaged on, not just this new one) rather
    // than leaving it running while you're now shaping something else.
    proc.setListenBand (-1);

    repaint();
}

void EQCurveDisplay::removeBand (int band)
{
    // New transaction boundary -- this writes directly (not through
    // beginGesture()), so without this it would otherwise silently merge
    // into whatever transaction the last drag/edit happened to leave open.
    proc.beginNewTransaction();

    // Return the band to the pool: fully off, un-muted.
    setBandParam (band, "mute", 0.0f);
    setBandParam (band, "on",   0.0f);

    // A deleted band can't stay "listened to".
    if (proc.getListenBand() == band)
        proc.setListenBand (-1);

    if (selectedBand == band)
        setSelectedBand (-1);

    repaint();
}

//==============================================================================
juce::StringArray EQCurveDisplay::getRangePresetChoices()
{
    juce::StringArray choices;
    for (float preset : kRangePresets)
        choices.add (juce::String ((int) preset) + " dB");
    return choices;
}

void EQCurveDisplay::pushHint (juce::String h)
{
    if (h == lastHint)
        return;

    lastHint = std::move (h);

    if (onHintChanged != nullptr)
        onHintChanged (lastHint);
}

void EQCurveDisplay::updateHint()
{
    if (menuOwnsHint)
        return;

    juce::String hint;

    // Mid-drag on the dynamic handle, the pill reports the range LIVE --
    // the one number this gesture is setting, which has no other readout.
    if (draggingDynBand >= 0)
    {
        const float rangeNow = bandDynRange (draggingDynBand);
        hint = "range " + juce::String (rangeNow, 1) + " dB";
    }
    else if (hoverPos.x >= 0.0f)
    {
        if (dynHandleAtPosition (hoverPos) >= 0)
        {
            hint = "drag to set dynamic range";
        }
        else if (rangeBubbleBounds().expanded (3.0f).contains (hoverPos))
        {
            hint = "vertical scale of the graph";
        }
        else if (listenHoldActive)
        {
            // Auditioning: the strip has no cells to describe.
        }
        else if (const int sb = hoverStripBand(); sb >= 0)
        {
            // Only the action-strip CELLS explain themselves. Hovering the
            // band dot itself deliberately stays silent, because the pill
            // is showing that band's values there instead -- more useful
            // than a description (see getHoverBand()).
            const auto strip = actionStripRect (sb);
            if (strip.expanded (2.0f, 6.0f).contains (hoverPos))
            {
                const int cells = stripCellCount (sb);
                const int cell  = juce::jlimit (0, cells - 1,
                    (int) ((hoverPos.x - strip.getX()) / (strip.getWidth() / (float) cells)));

                // Kept inside the pill's 28-character budget (see registerHint).
                if (slopeExpandedBand == sb)
                {
                    static const char* const kSlopeHints[] =
                        { "12 dB/oct", "24 dB/oct", "36 dB/oct", "48 dB/oct" };
                    hint = kSlopeHints[slopeForPickerCell (sb, cell)];
                }
                else
                {
                    const bool isPassBand = bandIsPass (sb);
                    const int slopeCell = ! isPassBand ? -1
                                        : bandType (sb) == EQBand::FilterType::HighPass ? 0 : cells - 1;

                    hint = cell == slopeCell ? "filter slope"
                         : (! isPassBand ? cell : (cell == 1 ? 0 : 1)) == 0
                                               ? "mute this band"
                                               : "click and hold to audition";
                }
            }
        }
    }

    pushHint (hint);
}

juce::Rectangle<float> EQCurveDisplay::actionStripRect (int band) const
{
    const auto dot = pointForBand (band);
    const float stripW = actionStripWidthFor (stripCellCount (band));
    const float x = juce::jlimit (4.0f, (float) getWidth() - stripW - 4.0f,
                                  dot.x - stripW * 0.5f);

    // Above the dot normally, but UNDER it when the dot sits below the
    // graph's centre line -- a cut band's icons drop into the emptier space
    // beneath instead of crowding the curve between the dot and the centre.
    // Both branches clamp inside the graph.
    const bool goBelow = dot.y > (float) getHeight() * 0.5f;

    // A dynamic band has TWO markers stacked on its frequency: the dot and
    // the range arrow, which sits above the dot while expanding and below
    // it while compressing. Measure from whichever one is furthest out on
    // the side the strip is already taking, so the icons clear both instead
    // of landing on the arrow.
    //
    // Flipping the strip to the arrow's opposite side was tried first and
    // was worse: an expanding band threw its icons below the dot, when the
    // ask was simply for them to sit above the higher curve.
    float anchor = dot.y;
    if (bandShowsDynamics (band))
    {
        const float handleY = dynHandlePoint (band).y;
        anchor = goBelow ? juce::jmax (anchor, handleY)
                         : juce::jmin (anchor, handleY);
    }

    const float y = goBelow
        ? juce::jmin ((float) getHeight() - kActionIconH - 4.0f, anchor + kActionStripLift)
        : juce::jmax (4.0f, anchor - kActionStripLift - kActionIconH);

    return { x, y, stripW, kActionIconH };
}

int EQCurveDisplay::hoverStripBand() const
{
    if (hoverPos.x < 0.0f)
        return -1;

    // Nearest hovered ON dot first (ANY type -- every band has an action
    // strip) -- same outer radius as the handles' magnetic swell, so the
    // strip appears exactly as the dot starts swelling toward the cursor.
    //
    // A dynamic band's RANGE ARROW counts as the same band's handle here:
    // it's the band's other grabbable marker, and with the strip now
    // anchored above/below it (see actionStripRect), approaching the arrow
    // from outside the dot's radius otherwise left the icons hidden with
    // no way to reach them but going back to the dot first.
    int best = -1;
    float bestDist = 34.0f;
    for (int b = 0; b < EQAudioProcessor::numBands; ++b)
    {
        if (! bandOn (b)) continue;

        float d = hoverPos.getDistanceFrom (pointForBand (b));

        if (bandShowsDynamics (b))
            d = juce::jmin (d, hoverPos.getDistanceFrom (dynHandlePoint (b)));

        if (d < bestDist) { bestDist = d; best = b; }
    }
    if (best >= 0)
        return best;

    // Otherwise: the strip the cursor is already inside (slightly expanded,
    // overlapping the dot's own radius, so the pointer can travel from dot
    // to strip without a dead zone where everything vanishes).
    for (int b = 0; b < EQAudioProcessor::numBands; ++b)
        if (bandOn (b) && actionStripRect (b).expanded (6.0f, 8.0f).contains (hoverPos))
            return b;

    return -1;
}

void EQCurveDisplay::showTypeMenu (int band, juce::Rectangle<int> screenAnchor)
{
    auto* typeChoice = dynamic_cast<juce::AudioParameterChoice*> (
        proc.apvts.getParameter (EQAudioProcessor::bandParamID (band, "type")));
    auto* slopeChoice = dynamic_cast<juce::AudioParameterChoice*> (
        proc.apvts.getParameter (EQAudioProcessor::bandParamID (band, "slope")));

    if (typeChoice == nullptr)
        return;

    const int currentType  = typeChoice->getIndex();
    const int currentSlope = juce::jlimit (0, 3, bandSlope (band));

    // One result id encodes BOTH the chosen type and (for pass filters) its
    // slope, so a single callback handles flat items and nested ones alike.
    auto makeId = [] (int type, int slope) { return 1 + type * 8 + slope; };

    juce::PopupMenu menu;

    // Rows push their name into the readout pill as you sweep them. A
    // SafePointer because the menu outlives this call and the editor can be
    // closed while it's still open.
    auto pushName = [safeThis = juce::Component::SafePointer<EQCurveDisplay> (this)]
                    (const juce::String& n)
    {
        if (safeThis != nullptr)
            safeThis->pushHint (n);
    };

    // Choice names come straight from the parameter definitions -- the one
    // source of truth for what types/slopes exist, so adding either to the
    // processor automatically lands here.
    for (int t = 0; t < typeChoice->choices.size(); ++t)
    {
        const bool isPassFilter = (t == (int) EQBand::FilterType::HighPass
                                || t == (int) EQBand::FilterType::LowPass);

        // Rows are the filter's SHAPE, not its name (see TypeMenuItem) --
        // the itemTitle argument still carries the real name so the menu
        // stays keyboard/accessibility navigable.
        if (isPassFilter && slopeChoice != nullptr)
        {
            // Slope is a property OF the pass filter ("High-Pass 24 dB/oct"
            // is one idea), so it nests under its type rather than living
            // in a second floating strip on the graph. Rows are steepness
            // knees (see SlopeMenuItem), mirrored to match this filter's
            // own direction; the real "24 dB/oct" name rides along as the
            // item title for keyboard/accessibility navigation.
            const bool mirrored = (t == (int) EQBand::FilterType::HighPass);

            auto slopeSub = std::make_unique<juce::PopupMenu>();
            for (int s = 0; s < slopeChoice->choices.size(); ++s)
            {
                auto row = std::make_unique<SlopeMenuItem> (
                    s, mirrored, t == currentType && s == currentSlope, slopeChoice->choices[s]);
                row->onHighlight = pushName;

                slopeSub->addCustomItem (makeId (t, s), std::move (row),
                                         nullptr, slopeChoice->choices[s]);
            }

            // The parent row is clickable too (itemResultID), keeping the
            // band's existing slope -- so switching to High-Pass is still
            // one click if you don't care which slope.
            // Lower case for the HINT only -- the item title below stays
            // the real parameter name. Lower-casing the parameter itself
            // would rename the host's automation lane and what preset files
            // record. Slope rows keep their own case ("12 dB/oct"): dB is a
            // unit, and "db" would just read as a typo.
            auto row = std::make_unique<TypeMenuItem> ((EQBand::FilterType) t,
                                                       t == currentType, true,
                                                       typeChoice->choices[t].toLowerCase());
            row->onHighlight = pushName;

            menu.addCustomItem (makeId (t, currentSlope), std::move (row),
                                std::move (slopeSub), typeChoice->choices[t]);
        }
        else
        {
            auto row = std::make_unique<TypeMenuItem> ((EQBand::FilterType) t,
                                                       t == currentType, false,
                                                       typeChoice->choices[t].toLowerCase()); // hint only
            row->onHighlight = pushName;

            menu.addCustomItem (makeId (t, 0), std::move (row),
                                nullptr, typeChoice->choices[t]);
        }
    }

    // Anchored wherever the caller's control is (the readout pill's shape
    // glyph), styled by the editor's LookAndFeel -- same treatment as the
    // range bubble's menu.

    // The menu owns the hint line until it closes -- otherwise the polled
    // hint (updateHint, on the timer) would immediately overwrite the row
    // name, since the cursor has left the graph for the menu's own window.
    menuOwnsHint = true;

    menu.setLookAndFeel (&getLookAndFeel());
    menu.showMenuAsync (juce::PopupMenu::Options()
                            .withTargetScreenArea (screenAnchor),
        [safeThis = juce::Component::SafePointer<EQCurveDisplay> (this), band] (int result)
        {
            // SafePointer, not a raw `this`: this callback still fires (with
            // 0) when the menu is dismissed because the editor closed under
            // it, and the very first thing it does now is release the hint
            // lock -- so unlike before, even the dismissed-without-choosing
            // path dereferences the display.
            if (safeThis == nullptr)
                return;

            // Hand the hint line back to updateHint(): clearing it here (as
            // opposed to just unlocking) is what makes the next tick re-push
            // whatever the cursor is actually over now.
            safeThis->menuOwnsHint = false;
            safeThis->pushHint ({});

            if (result <= 0)
                return;

            const int typeIdx  = (result - 1) / 8;
            const int slopeIdx = (result - 1) % 8;

            auto& self = *safeThis;

            self.beginGesture (band, "type");
            self.setBandParam (band, "type", (float) typeIdx);
            self.endGesture   (band, "type");

            const bool isPassFilter = (typeIdx == (int) EQBand::FilterType::HighPass
                                    || typeIdx == (int) EQBand::FilterType::LowPass);
            if (isPassFilter)
            {
                self.beginGesture (band, "slope");
                self.setBandParam (band, "slope", (float) slopeIdx);
                self.endGesture   (band, "slope");
            }

            // Migrated from the old dropdown's onChange: switching TO a
            // type with a lower Q ceiling (Low-Pass/High-Pass cap at
            // maxQFor) clamps a too-high stored Q right away, instead of
            // leaving a value that's inconsistent with what's audible
            // until the user happens to touch Q.
            const float maxQ = EQBand::maxQFor ((EQBand::FilterType) typeIdx);
            if (self.bandQ (band) > maxQ)
            {
                self.beginGesture (band, "q");
                self.setBandParam (band, "q", maxQ);
                self.endGesture   (band, "q");
            }

            self.setSelectedBand (band);
            self.repaint();
        });
}

void EQCurveDisplay::showRangeMenu()
{
    juce::PopupMenu menu;
    const auto choices = getRangePresetChoices();
    for (int i = 0; i < choices.size(); ++i)
        menu.addItem (i + 1, choices[i], true, i == rangePresetIndex);

    // Inherits the editor's LookAndFeel (this component is its child), so
    // the menu matches every other popup in the plugin. Anchored to the
    // bubble itself, like a dropdown opening from it.
    menu.setLookAndFeel (&getLookAndFeel());
    menu.showMenuAsync (juce::PopupMenu::Options()
                            .withTargetScreenArea (localAreaToGlobal (rangeBubbleBounds().toNearestInt())),
        [this] (int result)
        {
            if (result > 0)
                setRangePresetIndex (result - 1);
        });
}

void EQCurveDisplay::setRangePresetIndex (int index)
{
    // Purely a view preference: this only feeds gainToY/yToGain below, never
    // the "gain" parameter itself, so bands keep whatever value they had —
    // they just may render clipped to the edge until a wider range is picked.
    if (index < 0 || index >= kNumRangePresets || index == rangePresetIndex)
        return;

    rangePresetIndex = index;
    // verticalRangeDb is NOT snapped here any more -- it GLIDES toward the
    // new preset (see tickRangeGlide), so both the auto-expand-on-drag and
    // a manual bubble pick rescale the graph smoothly instead of in one
    // frame ("it snaps and is not smooth").
    repaint();
}

void EQCurveDisplay::setBackgroundDim (float amount)
{
    amount = juce::jlimit (0.0f, 1.0f, amount);
    if (amount == backgroundDim)
        return;

    backgroundDim = amount;
    repaint();
}

juce::Colour EQCurveDisplay::currentBackgroundColour() const
{
    return EQLookAndFeel::background().interpolatedWith (EQLookAndFeel::dimmedBackground(), backgroundDim);
}

void EQCurveDisplay::mouseDown (const juce::MouseEvent& e)
{
    // The dB range bubble claims its corner before any band logic runs --
    // otherwise a click there would try to CREATE a band under the bubble.
    // Any button opens the menu (a right-click on a control this small
    // shouldn't silently do nothing).
    if (rangeBubbleBounds().expanded (3.0f).contains (e.position))
    {
        showRangeMenu();
        return;
    }

    // Hover-strip clicks -- checked BEFORE the band logic, since the strips
    // float over empty graph where a click would otherwise CREATE a band.
    // Any x within a strip picks the nearest cell (no dead gaps between
    // glyphs). Parameter writes go through the same gesture/undo path as
    // every other manual band-parameter edit.
    {
        const int sb = hoverStripBand();

        // Action strip: power (mute toggle), headphones (listen toggle),
        // X (delete) -- acting directly on the HOVERED band, no need to
        // select it first (though power/listen do select it, same as the
        // slope strip, so row 1's readout follows what you just touched).
        if (sb >= 0 && actionStripRect (sb).expanded (6.0f, 8.0f).contains (e.position))
        {
            const auto strip = actionStripRect (sb);
            const int  cells = stripCellCount (sb);
            const int  cell  = juce::jlimit (0, cells - 1,
                (int) ((e.position.x - strip.getX()) / (strip.getWidth() / (float) cells)));

            if (slopeExpandedBand == sb)
            {
                // The expanded picker: each cell IS a slope. Stays expanded
                // after the click (the cursor is still inside), so the
                // current-slope highlight visibly moves to the pick.
                beginGesture (sb, "slope");
                setBandParam (sb, "slope", (float) slopeForPickerCell (sb, cell));
                endGesture   (sb, "slope");
                setSelectedBand (sb);
                repaint();
                return;
            }

            // Collapsed: the slope handle (HP first cell, LP last -- see
            // the paint mapping) expands on hover; a click just selects.
            const bool isPassBand = bandIsPass (sb);
            const int slopeCell = ! isPassBand ? -1
                                : bandType (sb) == EQBand::FilterType::HighPass ? 0 : cells - 1;

            const int act = cell == slopeCell ? -1
                          : ! isPassBand      ? cell
                          : cell == 1         ? 0
                                              : 1;

            if (act < 0)
            {
                setSelectedBand (sb);
            }
            else if (act == 0)
            {
                beginGesture (sb, "mute");
                setBandParam (sb, "mute", bandMuted (sb) ? 0.0f : 1.0f);
                endGesture   (sb, "mute");
                setSelectedBand (sb);
            }
            else
            {
                // Listen isn't an APVTS parameter (see EQAudioProcessor::
                // setListenBand), and it's MOMENTARY: engaged only while
                // the mouse button is held on this cell -- mouseUp() snaps
                // it back off ("click and hold to use the feature").
                // Dragging during the hold sweeps the band's frequency
                // (see mouseDrag/the header comment), so the freq gesture
                // opens now and closes on release.
                proc.setListenBand (sb);
                listenHoldActive  = true;
                listenHoldBand    = sb;
                listenHoldXOffset = e.position.x - freqToX (bandFreq (sb));
                beginGesture (sb, "freq");
                setSelectedBand (sb);
            }

            repaint();
            return;
        }

    }

    // The dynamic-range handle, checked before the dots -- it loses ties to
    // a dot inside dynHandleAtPosition itself, so this order just means "a
    // clear click on the ring wins over empty-space band creation".
    if (! e.mods.isPopupMenu())
    {
        if (const int db = dynHandleAtPosition (e.position); db >= 0)
        {
            setSelectedBand (db);
            draggingDynBand = db;
            beginGesture (db, "dynrange");
            return;
        }
    }

    const int b = bandAtPosition (e.position);

    // Right-click (or ctrl-click) on a point removes it and frees the band.
    if (e.mods.isPopupMenu())
    {
        if (b >= 0)
            removeBand (b);
        return;
    }

    if (b >= 0)
    {
        // Left-click an existing point: select it and start dragging.
        setSelectedBand (b);
        draggingBand = b;
        // Open gestures on the params we might move, so the host records the
        // whole drag as one automation move.
        beginGesture (b, "freq");
        beginGesture (b, "gain");
        return;
    }

    // Left-click empty space: create a new point, if a band is still free.
    const float clickFreq = juce::jlimit (minFreq, maxFreq, xToFreq (e.position.x));
    const int   newBand   = findUnusedBandNearest (clickFreq);
    if (newBand < 0)
        return; // all six bands already placed — a 7th click does nothing

    beginGesture (newBand, "freq");
    beginGesture (newBand, "gain");
    createBandAt (newBand, e.position);
    setSelectedBand (newBand);
    draggingBand = newBand;   // let the user drag the new point immediately
}

// Auto-expand the vertical view while a drag pushes past its edge (the
// Pro-Q behaviour: "dragging a curve outside the current range of the
// display, the range will expand automatically as needed"). Steps up the
// SAME preset ladder the bubble's menu uses -- 6 -> 12 -> 30 -- so grid,
// labels and bubble all follow for free, and the dragged handle stays
// pinned under the cursor (only the meaning of its position changes; the
// next drag frame simply reads the new mapping, in finer dB-per-pixel).
// EXPAND ONLY, and only from a live drag: the view never contracts on its
// own, and preset loads / host automation landing outside the range just
// clamp at the edge as before -- those aren't your hand on the control.
// The 0.05 margin makes it fire AT the edge, not after fighting it.
void EQCurveDisplay::expandRangeToShow (float gainDb)
{
    // Compared against the TARGET preset, not the animated value -- mid-
    // glide the animated range trails the target, and comparing against it
    // would ladder straight through 12 to 30 in one pull before the first
    // expansion had even finished drawing.
    if (std::abs (gainDb) > kRangePresets[rangePresetIndex] - 0.05f
        && rangePresetIndex < kNumRangePresets - 1)
        setRangePresetIndex (rangePresetIndex + 1);
}

void EQCurveDisplay::tickRangeGlide()
{
    // Display-only smoothing, same pattern as the listen walls' Q glide:
    // the PRESET is the state, this animated copy is what gainToY/yToGain
    // draw and drag through. Multiplicative lerp because range is a zoom
    // (6 -> 12 and 12 -> 30 should feel like the same move); 0.30/tick at
    // 30 Hz settles in ~250 ms. Snapped once within a rounding hair so the
    // glide actually ENDS rather than approaching forever.
    const float target = kRangePresets[rangePresetIndex];

    if (std::abs (verticalRangeDb - target) < 0.01f)
    {
        verticalRangeDb = target;
        return;
    }

    verticalRangeDb *= std::pow (target / verticalRangeDb, 0.30f);
    repaint();
}

void EQCurveDisplay::mouseDrag (const juce::MouseEvent& e)
{
    // Keep the axis readout + magnetic handle swell live during drags too,
    // including the readout's speed-based fade (same accumulation as
    // mouseMove).
    hoverPos = e.position;

    // Sweep-while-auditioning: dragging with the headphones cell held moves
    // the band's frequency (horizontal only -- gain stays put, this is for
    // hunting frequencies by ear, not reshaping). The offset captured at
    // mouseDown keeps the band where it was relative to the cursor instead
    // of jumping to it. The listen bandpass follows automatically -- the
    // audio thread re-reads the band's freq every block.
    if (listenHoldActive && listenHoldBand >= 0)
    {
        setBandParam (listenHoldBand, "freq",
                      juce::jlimit (minFreq, maxFreq, xToFreq (e.position.x - listenHoldXOffset)));
        repaint();
        return;
    }

    // Dragging the dynamic handle: VERTICAL only (the handle lives at its
    // band's frequency; horizontal movement is ignored rather than moving
    // the band). The dragged position IS the destination -- static + range
    // -- so range is simply the distance from the static gain.
    if (draggingDynBand >= 0)
    {
        const int b2 = draggingDynBand;
        const float range = juce::jlimit (-18.0f, 18.0f,
                                          yToGain (e.position.y) - bandGain (b2));
        setBandParam (b2, "dynrange", range);
        expandRangeToShow (bandGain (b2) + range); // the arrow's DESTINATION is what must stay visible
        repaint();
        return;
    }

    if (draggingBand < 0) { repaint(); return; } // hover-driven drawing still tracks the cursor
    const int b = draggingBand;

    setBandParam (b, "freq", juce::jlimit (minFreq, maxFreq, xToFreq (e.position.x)));

    // Vertical drag only means something for gain-bearing types.
    if (typeHasGain (bandType (b)))
    {
        const float gain = juce::jlimit (-18.0f, 18.0f, yToGain (e.position.y));
        setBandParam (b, "gain", gain);
        expandRangeToShow (gain);
    }

    repaint();
}

void EQCurveDisplay::mouseUp (const juce::MouseEvent&)
{
    // Momentary listen: releasing the button snaps the audition back off
    // (see the headphones cell in mouseDown). JUCE delivers mouseUp to the
    // component that received mouseDown even if the cursor has wandered
    // off it, so this can't get stuck by releasing elsewhere. The freq
    // gesture opened for sweep-while-holding closes here, so the whole
    // hold+sweep records as one automation move.
    if (listenHoldActive)
    {
        if (listenHoldBand >= 0)
            endGesture (listenHoldBand, "freq");

        listenHoldActive = false;
        listenHoldBand   = -1;
        proc.setListenBand (-1);
        repaint();
    }

    if (draggingBand >= 0)
    {
        endGesture (draggingBand, "freq");
        endGesture (draggingBand, "gain");
        draggingBand = -1;
    }

    if (draggingDynBand >= 0)
    {
        endGesture (draggingDynBand, "dynrange");
        draggingDynBand = -1;
    }
}

void EQCurveDisplay::mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& wheel)
{
    // Adjust Q of the band under the cursor (or the selected one).
    int b = bandAtPosition (e.position);
    if (b < 0) b = selectedBand;
    if (b < 0) return;

    // Multiplicative step so each notch feels the same across the log range.
    // Capped at maxQFor(type) -- Low-Pass/High-Pass can't be scrolled into
    // the resonant-peak range at all (see EQBand::maxQFor()'s own comment).
    const float dir    = (wheel.deltaY >= 0.0f ? 1.0f : -1.0f) * (wheel.isReversed ? -1.0f : 1.0f);
    const float newQ   = juce::jlimit (0.1f, EQBand::maxQFor (bandType (b)), bandQ (b) * std::pow (1.25f, dir));

    beginGesture (b, "q");
    setBandParam (b, "q", newQ);
    endGesture   (b, "q");

    setSelectedBand (b);
    repaint();
}
