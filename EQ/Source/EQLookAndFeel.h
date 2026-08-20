#pragma once

#include <JuceHeader.h>

//==============================================================================
/**
    Shared monochrome look for the whole editor, plus a from-scratch rotary
    knob style: a thin outline circle, a clock-hand tick showing the current
    position, and the value printed as text INSIDE the circle. No filled arc,
    no gradient — just those three elements, so it matches the flat black/
    white styling used everywhere else in the UI.

    One instance is shared by every control in the editor (see
    PluginEditor::lookAndFeel), so any future knob gets this style for free.

    Also forces every text element in the plugin — labels, the dropdown, the
    popup menu — to the house UI face (see uiFont()), via overridable
    LookAndFeel font getters.
*/
class EQLookAndFeel : public juce::LookAndFeel_V4
{
public:
    EQLookAndFeel();

    //==========================================================================
    // The AudioFlower palette, as tokens -- THE single place colours are
    // defined. Every draw call in the plugin references these (directly, or
    // through a per-instance setColour that was given one of these), never a
    // raw juce::Colours literal, so re-theming the whole plugin means
    // editing these four functions, not ~75 call sites (which is exactly how
    // the light -> dark flip below was done). The few deliberate exceptions
    // are EFFECT colours (the bypass pill's glow, the listen spotlight's
    // dark vignette) which stay literal at their call sites because they
    // don't invert with the theme -- each is commented where it lives.
    //
    // DARK theme, matching the AudioFlower site's own palette: warm
    // near-black ground, warm off-white ink, copper accent. (The plugin was
    // originally black-on-white; that whole identity flipped here, in these
    // four lines, once every call site referenced tokens.)
    //
    // background(): #201B16 -- the site's warm near-black (#1A1612) lifted
    //     ~25%, same channel ratios. NOT a neutral charcoal (deliberate
    //     distance from Pro-Q's grey) and NOT pure black (thin light strokes
    //     halate on #000). Depth was tuned by eye across three trials: the
    //     raw site value read slightly too dark, a ~50% lift (#27211B) was
    //     firmly rejected ("way better before"), this midpoint is the
    //     "something more subtle" landing spot.
    // dimmedBackground(): what background() blends TOWARD while bypassed
    //     (see PluginEditor's bypassDim / EQCurveDisplay::setBackgroundDim);
    //     also folded into the ComboBox fill via setDimAmount() below. On
    //     dark, "dimmed" means LIGHTENED toward a flat warm grey -- the UI
    //     washes out rather than darkens, same semantic as before with the
    //     direction inverted.
    // ink(): #CEC8BE, a muted warm parchment -- all foreground: text,
    //     grid, curve, icons, outlines. Almost always applied
    //     .withAlpha(...) for the faint-grey hierarchy. Never pure white --
    //     stepped down the site's neutral scale TWICE by eye (#F1EEE7
    //     glared, #E4E0D8 was "still too bright"); this sits halfway
    //     between the site's bone (#E4E0D8) and its muted grey (#B8AFA4).
    // accent(): AudioFlower copper #C9825E -- the 0 dB line, the selected
    //     band handle, and the pre/post boost ribbon. Deliberately NOT
    //     flooded across every control the way Pro-Q saturates its amber.
    static juce::Colour background()       noexcept { return juce::Colour (0xff201b16); }
    static juce::Colour dimmedBackground() noexcept { return juce::Colour (0xff4a453e); }
    static juce::Colour ink()              noexcept { return juce::Colour (0xffcec8be); }
    static juce::Colour accent()           noexcept { return juce::Colour (0xffc9825e); }

    void drawRotarySlider (juce::Graphics& g, int x, int y, int width, int height,
                           float sliderPosProportional, float rotaryStartAngle,
                           float rotaryEndAngle, juce::Slider& slider) override;

    juce::Font getLabelFont (juce::Label&) override;
    juce::Font getComboBoxFont (juce::ComboBox&) override;
    juce::Font getPopupMenuFont() override;

    // Same house-font treatment extended to the save-preset AlertWindow
    // (title, message, and its Save/Cancel TextButtons) -- otherwise it falls
    // back to JUCE's default look entirely, since a modal AlertWindow doesn't
    // automatically inherit whatever LookAndFeel is set on the editor unless
    // explicitly assigned (see PluginEditor::showSavePresetDialog()).
    juce::Font getAlertWindowTitleFont() override;
    juce::Font getAlertWindowMessageFont() override;
    juce::Font getAlertWindowFont() override;
    juce::Font getTextButtonFont (juce::TextButton&, int buttonHeight) override;

    // JUCE's default draws no border and a bulkier arrow; ours is a small
    // outline chevron (Tabler "chevron-down" proportions) instead, matching
    // this UI's otherwise all-outline icon set (mute/listen/delete/undo/
    // redo/save/preset-arrows are all outline Tabler icons too).
    void drawComboBox (juce::Graphics&, int width, int height, bool isButtonDown,
                       int buttonX, int buttonY, int buttonW, int buttonH,
                       juce::ComboBox&) override;

    // JUCE's default reserves a fixed 30px on the right for its own (bigger)
    // arrow, regardless of what drawComboBox actually draws -- with our
    // small solid triangle and compact box widths, that left too little room
    // for the text and truncated it (e.g. "12 dB" -> "..."). Reserve less.
    void positionComboBoxText (juce::ComboBox&, juce::Label&) override;

    // JUCE's default always left-aligns menu item text; our dropdowns (Bell/
    // Low Shelf/.../6 dB/12 dB/...) have no icons, ticks, or submenus, so a
    // simplified centred version is all that's needed here.
    void drawPopupMenuItem (juce::Graphics& g, const juce::Rectangle<int>& area,
                            bool isSeparator, bool isActive, bool isHighlighted, bool isTicked,
                            bool hasSubMenu, const juce::String& text, const juce::String& shortcutKeyText,
                            const juce::Drawable* icon, const juce::Colour* textColour) override;

    // Plain white fill + a thin black border around every popup menu (both
    // ComboBox dropdowns and the hand-built preset menu, see PluginEditor::
    // PresetMenuItem) -- JUCE's default V4 background is a soft near-
    // invisible shadow with no crisp outline, which read as visually
    // inconsistent against the preset menu's own bordered "panel" look.
    void drawPopupMenuBackground (juce::Graphics&, int width, int height) override;

    // JUCE sizes each plain-text menu item (Bell/Low Shelf/... etc) using its
    // own default height/width logic, which came out noticeably taller/wider
    // than the preset menu's hand-set 22px rows -- matching that height here
    // (and computing width from the SAME uiFont() the preset menu uses)
    // is what actually unifies the two menus' look, not just the font.
    void getIdealPopupMenuItemSize (const juce::String& text, bool isSeparator, int standardMenuItemHeight,
                                    int& idealWidth, int& idealHeight) override;

    // The graph's background is a subtle vertical gradient (lighter at the
    // top, sinking toward black at the bottom) rather than a flat fill, so
    // it reads as a lit surface. Anything that needs to sit ON that surface
    // and disappear into it -- the dB-range bubble, the readout pill, the
    // trim pill -- must fill with the gradient's shade AT ITS OWN HEIGHT,
    // not with the flat token.
    //
    // `base` is the flat background for the current bypass-dim state (the
    // caller passes it, since only the graph tracks that fade), and
    // `verticalFraction` is 0 at the top of the graph, 1 at the bottom.
    //
    // This lives here, in one place, because the two gradient constants
    // were originally duplicated between EQCurveDisplay's own background
    // fill and the bubble drawn on top of it -- and the pills built later
    // filled flat instead, so they read visibly darker near the top of the
    // graph and lighter near the bottom.
    static juce::Colour surfaceAt (juce::Colour base, float verticalFraction) noexcept
    {
        const auto top    = base.interpolatedWith (ink(), 0.05f);
        const auto bottom = base.interpolatedWith (juce::Colours::black, 0.22f);
        return top.interpolatedWith (bottom, juce::jlimit (0.0f, 1.0f, verticalFraction));
    }

    // Width reserved at each side of a popup-menu row for its state marks
    // (tick at the left, submenu chevron at the right) -- see
    // drawPopupMenuItem()/getIdealPopupMenuItemSize().
    static constexpr int kMenuMarkZone = 14;

    // Corner radius of a popup menu -- see drawPopupMenuBackground(), and
    // the constructor for the transparency this depends on.
    //
    // Deliberately much tighter than the other floating surfaces (the pills
    // are stadiums at height/2, the guide panel is 10). Menu rows are only
    // 22-26px tall, so a large radius would bite into the first and last
    // rows; this reads as the same family without fighting the geometry.
    //
    // 5 is also inside the limit where row highlights can't poke out past
    // the rounded edge. A row is inset by getPopupMenuBorderSize() (2) plus
    // its own reduced(1), and a corner at inset `b` clears a radius-r arc
    // while b >= r * (1 - 1/sqrt(2)), i.e. r <= ~6.8 at b = 2. So no row
    // needs to know the menu has corners at all -- raise this past ~6 and
    // they would.
    static constexpr float kMenuCornerRadius = 5.0f;

    // Shared helper so code that draws text directly with Graphics (e.g. the
    // curve display's axis labels, which aren't routed through a LookAndFeel
    // at all) can still match this same typeface.
    static juce::Font uiFont (float height);

    // The editor's corner-resizer grip, redrawn in the house style: the
    // classic three diagonal lines, but in ink at 25% alpha so it reads as
    // a hint rather than clutter (JUCE's stock glyph was tried and looked
    // bad; fully hiding it was tried next and the affordance was missed --
    // faint is the landing spot).
    void drawCornerResizer (juce::Graphics&, int w, int h, bool isMouseOver, bool isMouseDragging) override;

    // Bypass-dim fade (0 = normal, 1 = fully dimmed), pushed here each timer
    // tick from PluginEditor alongside EQCurveDisplay::setBackgroundDim() --
    // the dropdowns' own hardcoded white background (set per-instance via
    // setColour) never blended with the rest of the UI's dimming on its own,
    // since drawComboBox previously used that colour raw. Routed through the
    // shared LookAndFeel so any current/future ComboBox picks it up for free.
    void setDimAmount (float amount) noexcept { dimAmount = amount; }

private:
    float dimAmount = 0.0f;
};
