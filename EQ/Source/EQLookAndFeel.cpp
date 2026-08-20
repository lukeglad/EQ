#include "EQLookAndFeel.h"

//==============================================================================
EQLookAndFeel::EQLookAndFeel()
{
    // A ColourScheme is the one place that also reaches things individual
    // setColour() calls on a component can't touch — e.g. a ComboBox's popup
    // menu, which is a separate top-level component that reads its colours
    // from whatever LookAndFeel is active when it's shown.
    // Built from the palette tokens (see the header) -- note the semantic
    // mapping, not a colour-by-colour copy: highlighted rows swap the two
    // (ink fill, background text), which keeps menu selection legible
    // automatically whichever way the theme flips.
    const juce::LookAndFeel_V4::ColourScheme mono
    {
        background(),                    // windowBackground
        background(),                    // widgetBackground
        background(),                    // menuBackground
        ink(),                           // outline
        ink(),                           // defaultText
        ink(),                           // defaultFill
        background(),                    // highlightedText
        ink(),                           // highlightedFill
        ink()                            // menuText
    };
    setColourScheme (mono);

    // Re-set AFTER the scheme (which mapped menuBackground to this) with one
    // alpha step off opaque, purely to flip the menu window's transparency:
    // PopupMenu::MenuWindow does
    //     setOpaque (lf.findColour (backgroundColourId).isOpaque() || ...)
    // and an opaque window must paint every pixel -- which a rounded
    // background by definition doesn't, leaving its four corners undefined.
    // Non-opaque, they're genuinely transparent, and because menus carry
    // ComponentPeer::windowHasDropShadow the OS shadow is cast from that
    // alpha too, so it rounds along with them.
    //
    // The value itself is never drawn (drawPopupMenuBackground fills with
    // background() directly, and this ID is read nowhere else in JUCE) --
    // it's kept visually identical anyway so nothing surprising appears if
    // that ever stops being true.
    setColour (juce::PopupMenu::backgroundColourId, background().withAlpha (0.99f));
}

//==============================================================================
void EQLookAndFeel::drawCornerResizer (juce::Graphics& g, int w, int h, bool, bool)
{
    // Three diagonal grip lines hugging the bottom-right corner, thin and
    // faint (see the header comment for the tried-and-rejected history).
    g.setColour (ink().withAlpha (0.25f));
    for (int i = 1; i <= 3; ++i)
    {
        const float offset = (float) i * ((float) w * 0.25f);
        g.drawLine ((float) w - offset, (float) h, (float) w, (float) h - offset, 1.0f);
    }
}

juce::Font EQLookAndFeel::uiFont (float height)
{
    // IBM Plex Mono -- SETTLED, embedded (OFL), after trialling seven faces
    // whole-UI against the dark theme: Times New Roman (the original),
    // Indie Flower, Inter, Space Mono, Courier Prime and DM Mono.
    //
    // Monospace is a FUNCTIONAL choice as much as a stylistic one: this UI
    // is almost entirely numbers, and a proportional face makes the
    // readout's digits shift width as values change while dragging (which
    // is why layoutReadoutLabels() re-packs every frame). Mono digits hold
    // their column. Plex specifically won on legibility at the 10-11px
    // sizes this UI is full of, and it was drawn with humanist warmth on
    // purpose -- so it contrasts the hand-lettered Indie Flower wordmark
    // (a machined instrument wrapped in a handmade brand) without going
    // cold the way a neutral grotesque did.
    //
    // Runners-up, for the record: Courier Prime paired most beautifully
    // with the wordmark (typewriter + handwriting are both analog writing
    // tools) but is the widest face, and text-measured widths here (type
    // menu rows, readout pill, combo boxes) grow with it; DM Mono was the
    // most condensed and quietest. Both were removed from Resources/ once
    // this was decided -- re-add the .ttf + a .jucer entry to revisit.
    static const juce::Typeface::Ptr face = juce::Typeface::createSystemTypefaceFor (
        BinaryData::IBMPlexMonoRegular_ttf, (size_t) BinaryData::IBMPlexMonoRegular_ttfSize);

    return juce::Font (juce::FontOptions (height).withTypeface (face));
}

juce::Font EQLookAndFeel::getLabelFont (juce::Label& label)
{
    // Every label in this plugin renders in the house UI face (see
    // uiFont()) regardless of
    // whatever font was set on it directly -- LookAndFeel_V2::drawLabel()
    // always renders via getLabelFont(), never label.getFont(), so a plain
    // label.setFont() call is otherwise silently ignored. The brand-mark
    // label is the one deliberate exception (Indie Flower, not Times), so
    // it's exempted here by component ID instead of forced through this
    // override like everything else.
    if (label.getComponentID() == "logoLabel")
        return label.getFont();

    return uiFont (LookAndFeel_V4::getLabelFont (label).getHeight());
}

juce::Font EQLookAndFeel::getComboBoxFont (juce::ComboBox& box)
{
    return uiFont (LookAndFeel_V4::getComboBoxFont (box).getHeight());
}

juce::Font EQLookAndFeel::getPopupMenuFont()
{
    // Fixed 14 -- was deriving its height from JUCE's own default popup menu
    // font (17px), which made the type/dB dropdowns' text noticeably bigger
    // than the preset menu's (PresetMenuItem::paint() hardcodes 14 directly).
    // Both now match.
    return uiFont (14.0f);
}

juce::Font EQLookAndFeel::getAlertWindowTitleFont()
{
    return uiFont (LookAndFeel_V4::getAlertWindowTitleFont().getHeight());
}

juce::Font EQLookAndFeel::getAlertWindowMessageFont()
{
    return uiFont (LookAndFeel_V4::getAlertWindowMessageFont().getHeight());
}

juce::Font EQLookAndFeel::getAlertWindowFont()
{
    return uiFont (LookAndFeel_V4::getAlertWindowFont().getHeight());
}

juce::Font EQLookAndFeel::getTextButtonFont (juce::TextButton& button, int buttonHeight)
{
    return uiFont (LookAndFeel_V4::getTextButtonFont (button, buttonHeight).getHeight());
}

void EQLookAndFeel::drawComboBox (juce::Graphics& g, int width, int height, bool /*isButtonDown*/,
                                  int /*buttonX*/, int /*buttonY*/, int /*buttonW*/, int /*buttonH*/,
                                  juce::ComboBox& box)
{
    auto bounds = juce::Rectangle<int> (0, 0, width, height).toFloat();

    // No outline -- just the fill, so the type/dB dropdowns read as plain
    // text with a caret rather than a boxed control (kept the fill so
    // bypass-dim still blends correctly against the graph's own background).
    g.setColour (box.findColour (juce::ComboBox::backgroundColourId)
                    .interpolatedWith (dimmedBackground(), dimAmount));
    g.fillRect (bounds);

    // Outline chevron (Tabler Icons "chevron-down" proportions, 12 wide x 6
    // tall in its native 24x24 space) instead of a solid filled triangle --
    // every OTHER glyph in this plugin (mute/listen/delete, undo/redo, save,
    // the preset prev/next arrows) is an outline-stroke Tabler icon, so the
    // dropdown's own arrow was the one mismatched solid shape.
    const float arrowW = juce::jmin (8.0f, bounds.getWidth() * 0.18f);
    const float arrowH = arrowW * 0.5f; // matches chevron-down's 12:6 ratio
    const float cx = bounds.getRight() - arrowW * 1.4f;
    const float cy = bounds.getCentreY();

    juce::Path arrow;
    arrow.startNewSubPath (cx - arrowW * 0.5f, cy - arrowH * 0.5f);
    arrow.lineTo          (cx,                 cy + arrowH * 0.5f);
    arrow.lineTo          (cx + arrowW * 0.5f, cy - arrowH * 0.5f);

    g.setColour (box.findColour (juce::ComboBox::arrowColourId));
    g.strokePath (arrow, juce::PathStrokeType (1.2f, juce::PathStrokeType::curved,
                                               juce::PathStrokeType::rounded));
}

void EQLookAndFeel::positionComboBoxText (juce::ComboBox& box, juce::Label& label)
{
    // Base class (LookAndFeel_V4) hardcodes box.getWidth() - 30 here, sized
    // for its own bigger default arrow. Our arrow is a small solid triangle
    // (see drawComboBox above), so reserve much less -- otherwise the text
    // area is narrower than the box widths we've deliberately sized to just
    // fit the content, and it gets ellipsised (e.g. "12 dB" -> "...").
    constexpr int arrowZone = 16;
    label.setBounds (1, 1, box.getWidth() - arrowZone, box.getHeight() - 2);
    label.setFont (getComboBoxFont (box));
}

void EQLookAndFeel::drawPopupMenuItem (juce::Graphics& g, const juce::Rectangle<int>& area,
                                       bool isSeparator, bool isActive, bool isHighlighted, bool isTicked,
                                       bool hasSubMenu, const juce::String& text,
                                       const juce::String& /*shortcutKeyText*/,
                                       const juce::Drawable* /*icon*/, const juce::Colour* textColour)
{
    // Our menus are plain text lists -- no icons or shortcut keys -- so this
    // only handles separator/highlight/text, centred instead of JUCE's
    // default left-aligned layout, PLUS the two state marks drawn in the
    // side margins getIdealPopupMenuItemSize() reserves: a tick at the left
    // for the current selection, and a chevron at the right for a submenu
    // parent. Both were ignored here originally (the menus had no ticked or
    // nested items); the band-type menu now needs both, since it carries
    // the current type AND nests each pass filter's slope choices.
    if (isSeparator)
    {
        auto r = area.reduced (5, 0);
        r.removeFromTop (juce::roundToInt (((float) r.getHeight() * 0.5f) - 0.5f));
        g.setColour (findColour (juce::PopupMenu::textColourId).withAlpha (0.3f));
        g.fillRect (r.removeFromTop (1));
        return;
    }

    auto r = area.reduced (1);

    if (isHighlighted && isActive)
    {
        g.setColour (findColour (juce::PopupMenu::highlightedBackgroundColourId));
        g.fillRect (r);
        g.setColour (findColour (juce::PopupMenu::highlightedTextColourId));
    }
    else
    {
        const auto colour = textColour != nullptr ? *textColour : findColour (juce::PopupMenu::textColourId);
        g.setColour (colour.withMultipliedAlpha (isActive ? 1.0f : 0.5f));
    }

    // Text stays centred in the FULL item width (not in the space left over
    // between the marks), so every row's text lines up whether or not it
    // carries a tick or a chevron.
    g.setFont (getPopupMenuFont());
    g.drawFittedText (text, r, juce::Justification::centred, 1);

    if (isTicked)
    {
        // Same "✓" convention the preset menu's own custom rows use.
        g.setFont (getPopupMenuFont());
        g.drawText (juce::String (juce::CharPointer_UTF8 ("\xE2\x9C\x93")),
                    r.removeFromLeft (kMenuMarkZone), juce::Justification::centred);
    }

    if (hasSubMenu)
    {
        // Outline chevron pointing right -- the same Tabler "chevron"
        // proportions and stroke weight as the ComboBox arrow, rotated, so
        // menus and dropdowns share one glyph language.
        auto zone = area.reduced (1).removeFromRight (kMenuMarkZone).toFloat();
        const float h = juce::jmin (7.0f, zone.getHeight() * 0.5f);
        const float w = h * 0.5f;
        const float cx = zone.getCentreX(), cy = zone.getCentreY();

        juce::Path chevron;
        chevron.startNewSubPath (cx - w * 0.5f, cy - h * 0.5f);
        chevron.lineTo          (cx + w * 0.5f, cy);
        chevron.lineTo          (cx - w * 0.5f, cy + h * 0.5f);
        g.strokePath (chevron, juce::PathStrokeType (1.2f, juce::PathStrokeType::curved,
                                                     juce::PathStrokeType::rounded));
    }
}

void EQLookAndFeel::drawPopupMenuBackground (juce::Graphics& g, int width, int height)
{
    // NOT fillAll: the corners are meant to stay transparent (see the
    // constructor), which is the whole point of the rounding.
    const auto r = juce::Rectangle<int> (0, 0, width, height).toFloat().reduced (0.5f);

    g.setColour (background());
    g.fillRoundedRectangle (r, kMenuCornerRadius);

    // Hairline at the SAME weight every other floating surface in this
    // plugin uses -- the readout/trim pills, the dB bubble and the guide
    // panel are all ink at 40%. This was full-opacity ink, which read as a
    // system dialog dropped onto the UI rather than part of it.
    g.setColour (ink().withAlpha (0.40f));
    g.drawRoundedRectangle (r, kMenuCornerRadius, 1.0f);
}

void EQLookAndFeel::getIdealPopupMenuItemSize (const juce::String& text, bool isSeparator,
                                               int /*standardMenuItemHeight*/,
                                               int& idealWidth, int& idealHeight)
{
    if (isSeparator)
    {
        idealWidth = 0;
        idealHeight = 6;
        return;
    }

    // Reserve a mark zone on BOTH sides so the centred text is never
    // crowded by a tick (left) or submenu chevron (right) -- see
    // drawPopupMenuItem. Symmetric whether or not a given row has either,
    // so rows in the same menu stay the same width.
    idealWidth = juce::roundToInt (juce::GlyphArrangement::getStringWidth (uiFont (14.0f), text))
               + 16 + kMenuMarkZone * 2;
    idealHeight = 22;
}

void EQLookAndFeel::drawRotarySlider (juce::Graphics& g, int x, int y, int width, int height,
                                      float sliderPosProportional, float rotaryStartAngle,
                                      float rotaryEndAngle, juce::Slider& slider)
{
    const auto bounds = juce::Rectangle<int> (x, y, width, height).toFloat().reduced (2.0f);

    // Knob draws at ~65% of the available diameter, anchored towards the top
    // of its bounds (i.e. right under the label above it) rather than
    // vertically centred — that's what actually tightens the visual gap to
    // the label, since the slider's own component bounds don't shrink.
    const float radius = juce::jmin (bounds.getWidth(), bounds.getHeight()) * 0.5f * 0.65f;
    const juce::Point<float> centre { bounds.getCentreX(), bounds.getY() + radius + 2.0f };

    const auto outlineColour = slider.findColour (juce::Slider::rotarySliderOutlineColourId);
    const auto tickColour    = slider.findColour (juce::Slider::thumbColourId);

    // 1. The knob body: a plain outline circle, no fill. Thin stroke.
    g.setColour (outlineColour);
    g.drawEllipse (centre.x - radius, centre.y - radius, radius * 2.0f, radius * 2.0f, 1.0f);

    // 2. The tick: a short mark near the rim only (not a spoke to the
    // centre), angled for the current value between the slider's start/end
    // rotary angles.
    const float angle = rotaryStartAngle + sliderPosProportional * (rotaryEndAngle - rotaryStartAngle);
    const juce::Point<float> tickStart = centre.getPointOnCircumference (radius * 0.72f, angle);
    const juce::Point<float> tickEnd   = centre.getPointOnCircumference (radius * 0.95f, angle);
    g.setColour (tickColour);
    g.drawLine ({ tickStart, tickEnd }, 1.0f);

    // 3. The value, centred inside the (now smaller, off-centre) circle, as
    // two stacked lines — number on top, unit below — rather than one line.
    // No separate JUCE text box — the caller sets Slider::NoTextBox and we
    // split/draw the same getTextFromValue() string here ourselves.
    const juce::String suffix = slider.getTextValueSuffix();      // e.g. " dB"
    juce::String fullText = slider.getTextFromValue (slider.getValue());
    juce::String valueText = fullText.endsWith (suffix)
                                ? fullText.dropLastCharacters (suffix.length())
                                : fullText;
    valueText = valueText.trim();
    const juce::String unitText = suffix.trim();

    const float fontSize = juce::jmax (9.0f, radius * 0.5f);
    g.setFont (uiFont (fontSize));
    g.setColour (tickColour);

    const float lineHeight = fontSize * 1.15f;
    const float textWidth  = radius * 1.8f;
    const auto  valueLine  = juce::Rectangle<float> (textWidth, lineHeight)
                                .withCentre ({ centre.x, centre.y - lineHeight * 0.5f });
    const auto  unitLine   = juce::Rectangle<float> (textWidth, lineHeight)
                                .withCentre ({ centre.x, centre.y + lineHeight * 0.5f });

    g.drawFittedText (valueText, valueLine.toNearestInt(), juce::Justification::centred, 1);
    g.drawFittedText (unitText,  unitLine.toNearestInt(),  juce::Justification::centred, 1);
}
