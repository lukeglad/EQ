# EQ — parametric EQ plugin (JUCE)

6-band parametric EQ VST3/AU for the user's portfolio, built with JUCE 8 (Projucer + Xcode),
tested in FL Studio 2025.

> **READ THIS FIRST — 2026-08-18 redesign.** The plugin is now a DARK theme: warm near-black
> ground (#201B16), muted parchment ink (#CEC8BE), copper accent (#C9825E) — all defined as
> palette TOKENS on `EQLookAndFeel` (`background()/dimmedBackground()/ink()/accent()`); no draw
> call uses raw colour literals except commented effect-colour exceptions. The window is
> USER-RESIZABLE (drag the faint corner grip): every control lives on a 720x539 design-space
> `ContentComponent` scaled by one transform — resize-by-scale, not reflow; default 936x701,
> aspect-locked, limits 0.75x–2x. Chosen size persists via `SizeMemoryConstrainer` (ONLY
> corner-drag resizes are recorded — FL pushes stale sizes on reopen that must never overwrite
> the memory) + an `editorWidth` attribute in session state (stripped before replaceState,
> never in preset files) + a deferred re-apply in the editor ctor. Do NOT reintroduce
> `setScaleFactor` — it was replaced by this architecture. Filter types now include NOTCH;
> the FilterType enum is APPEND-ONLY (indices live in saved state). The analyzer-skin system,
> grain texture, and ambient-video background are all DELETED (video was built and rejected
> TWICE — do not re-suggest it).
> Copper is used in exactly three roles (0 dB line, selected handle, boost ribbon) — deliberately
> NOT flooded Pro-Q-style; the ONE exception is the guide overlay's site link. **There is no chrome
> left but the top bar: the bottom control section is DELETED and the graph fills everything under
> it.** Top bar = global controls only (undo, redo, ‹preset name›, bypass, "?"). Everything else
> floats ON the graph: hover a band's dot for its 4-icon action strip (shape → type menu with slope
> submenus, power/mute, momentary hold-to-listen with drag-to-sweep, delete) plus that band's own
> frequency drawn above it; the freq/gain/Q readout pill (top-centre, real Labels so double-click-
> to-type survives); the dB-range bubble (top-left); a SINGLE In/Out trim pill (bottom-left — its
> caption is a switch, defaults to Out, drag to adjust / double-click to type, bound via
> ParameterAttachment not SliderAttachment); and the AudioFlower watermark. **Saving lives in the
> preset menu** (left-click the name → "Save as…" + list; right-click → straight to save) — there
> is no save button. The "?" opens a self-sizing guide overlay. There is NO cursor-frequency
> readout in the axis row any more; the band's frequency above the hover strip replaced it.
>
> **Gotchas this layout has already caused twice** — JUCE paints/hit-tests children in INSERTION
> order: the watermark (added late, full graph width) silently blanketed every control beneath it
> until `setInterceptsMouseClicks(false,false)`, and the guide overlay opened invisibly behind the
> graph until it was added LAST and given `toFront()`. Check both for anything full-width added to
> `content`.
>
> **`EQLookAndFeel::surfaceAt(base, verticalFraction)` is the single source** of the graph's
> background-gradient shade — every pill fills through it so it blends at its own height. Filling
> with the flat `background()` token instead makes a pill read darker near the graph's top and
> lighter near its bottom.
>
> A level METER was built (processor peak taps, ballistics, latched clip) and then deleted outright
> as distracting — including its processBlock taps. Do not re-add it unprompted, same as the
> ambient-video background.
>
> Graph background is a subtle vertical gradient (lit-panel look; user still undecided on its
> empty-state feel). NOTE: `EQLookAndFeel::drawPopupMenuItem` draws the ✓/submenu-chevron marks in
> reserved side margins (`kMenuMarkZone`) — menus depend on it for showing the current selection.
>
> **Font: IBM Plex Mono — settled.** All text routes through the single `EQLookAndFeel::uiFont()`
> (renamed from `timesFont()` once Times was dropped). SEVEN faces were trialled whole-UI against
> the dark theme — Times New Roman (the original), Indie Flower, Inter, Space Mono, Courier Prime,
> DM Mono — and Plex won: monospace keeps the readout's digits from shifting width while dragging
> (this UI is almost all numbers), it's the most legible of the monos at 10–11px, and its humanist
> warmth contrasts the hand-lettered wordmark without going cold. Only TWO fonts are now embedded:
> `IBMPlexMono-Regular.ttf` (all UI) and `IndieFlower-Regular.ttf` (the "AudioFlower" wordmark only,
> via the `logoLabel` exemption — brand marks only, the standing brand rule). The other five were
> deleted from `Resources/` and the `.jucer`; re-add a `.ttf` + entry to revisit one. Sections below describing the black-on-white look, skins, or grain
> predate this and are kept as history — trust the token layer and current source over them.
> Sibling brand module at `~/Documents/AudioFlower-Brand/SharedUI/` is kept value-identical to
> EQLookAndFeel's tokens; the reference-sheet Artifact still documents the LIGHT identity (stale,
> needs a redo). GitHub repo for this project is scoped but NOT started (gh not installed).

## Build / test workflow (do this after every code change)

- **Project root:** `/Users/lukeglad/Documents/EQ/EQ/` (nested one level so the `.jucer`'s
  `../../JUCE/modules` paths resolve to `/Users/lukeglad/Documents/JUCE`).
- **Regenerate Xcode project** after adding/removing source files or editing the `.jucer`:
  `/Users/lukeglad/Documents/JUCE/Projucer.app/Contents/MacOS/Projucer --resave EQ.jucer`
  (No resave needed for edits to existing files.)
- **Build:** `cd Builds/MacOSX && xcodebuild -project EQ.xcodeproj -scheme "EQ - All" -configuration Debug build`
- **Auto-installs** on build to `~/Library/Audio/Plug-Ins/{VST3,Components}` via the copy step.
- **Validate the AU:** `auval -v aufx Peq1 Lgld` (plugin codes: manufacturer `Lgld`, plugin `Peq1`).
  Its render tests exercise the audio-thread path — a good RT-safety signal.
- **Standard: builds are warning-free.** Treat any new warning as something to fix.
- FL Studio does NOT hot-reload: after a rebuild the user must remove + re-add the plugin
  instance (or reload the project) to pick up the new binary.

## Source files (all in `Source/`)

- `PluginProcessor.{h,cpp}` — `EQAudioProcessor`. APVTS (single source of truth), 6-band chain,
  in/out gain trims, owns the `SpectrumAnalyzer`. `bandParamID(band, name)` builds IDs like `b2_freq`.
- `EQBand.{h,cpp}` — one reusable filter band. Static `makeCoefficients(...)` is the single source
  of the biquad math, shared by DSP and the UI curve.
- `EQCurveDisplay.{h,cpp}` — the interactive graph (curve, points, grid, axis labels, analyzer draw).
- `PluginEditor.{h,cpp}` — 720x539 DESIGN-space layout on a scaled `ContentComponent` (see the
  read-me block above). Bottom control section is now ONE row (In/Out, logo, bypass); the
  per-band panel that used to sit above it is gone entirely. Owns the `EQLookAndFeel`.
- `EQLookAndFeel.{h,cpp}` — custom monochrome LookAndFeel (see UI style below).
- `SpectrumAnalyzer.{h,cpp}` — real-time FFT analyzer (see below).

## DSP / parameters

- Per band (i=0..5): `b{i}_on` (bool, PLACED — has a point at all), `b{i}_type` (choice: Bell/Low
  Shelf/High Shelf/High-Pass/Low-Pass — order MUST match `EQBand::FilterType`), `b{i}_freq`,
  `b{i}_gain` (±18 dB), `b{i}_q`, `b{i}_solo` (bool, no UI, still host-automatable), `b{i}_mute`
  (bool — DISTINCT from `on`: bypasses that band's DSP while the point/settings stay exactly where
  they were; deleting clears `on` and removes the point, muting never touches `on`). Global: `inGain`,
  `outGain` (±24 dB), **`bypass`** (bool — re-added this session after an earlier full removal; see
  below).
- Each band = a `dsp::ProcessorDuplicator<IIR::Filter, IIR::Coefficients>` for stereo. Coefficients
  rebuilt once per block from `SmoothedValue` targets (multiplicative for freq/Q, linear-dB for gain)
  — no zipper noise. Signal flow: `if (bypass) return;` (true hard bypass, right after
  `ScopedNoDenormals`, before anything else) -> inputGain -> band0..5 (series, `active = on && !mute &&
  (!anySolo || solo)`) -> outputGain -> post-EQ analyzer tap -> **listen override** (see below).
- Solo: if any band is soloed, only soloed bands process. **`solo` still exists as a param but has NO
  UI control** (removed) — it's still host-automatable.
- **Bypass** reports itself as the host's native/generic bypass via
  `EQAudioProcessor::getBypassParameter()`. UI: a pill/slider toggle (`PluginEditor::PillToggle`,
  rounded track + sliding thumb) in the bottom-right of row 2, next to the dB-range dropdown.
- **"Listen" (audition), NOT an APVTS parameter (`EQAudioProcessor::getListenBand()`/`setListenBand()`,
  plain atomic int, -1 = off):** applies a `dsp::IIR::Coefficients::makeBandPass` filter (centred at
  that band's own freq, width from its own Q) to a SEPARATE copy of the pre-band signal
  (`listenScratch`), captured right after the input trim, and swaps it in as the FINAL output ONLY
  while active — this is the very last thing `processBlock` does, so it never touches the real
  per-band processing; turning it off leaves the actual EQ'd output completely unaffected. Only one
  band can be listened to at a time (a single stored index). Deliberately not host-automatable/saved
  in a preset — a temporary monitoring aid only.

## Interaction model — click-to-create (Pro-Q style)

- A band is "placed" iff its `on` param is true. Graph starts flat/empty.
- **Left-click empty graph** = wake the nearest-freq unused band (Bell, Q 1, freq/gain from click),
  set `on`=1. Max 6; a click with all 6 placed is a no-op.
- **Left-click a point** = select + drag it (drag X=freq, Y=gain; scroll=Q).
- **Right-click a point** = remove (sets `solo`=0 THEN `on`=0 — clearing solo is REQUIRED, or a
  soloed-off band mutes the whole EQ).
- The UI reads/writes the SAME APVTS params the audio thread uses, so every mouse edit is identical
  to a host automation move.

## Spectrum analyzer architecture (`SpectrumAnalyzer`)

Real-time PRE-EQ spectrum, drawn as a light-grey fill BEHIND the black EQ curve.

- **Owned by the processor** (not the editor) so the audio thread always has a valid push target.
- **Dual-FFT (two instances):** `SpectrumAnalyzer`'s FFT order is now a CONSTRUCTOR param (was a fixed
  constant), so `EQAudioProcessor` owns two: `analyzer` (fast, order 12/4096, mids/highs) and
  `analyzerLow` (slow, order 13/8192, finer bin spacing, low end only). Both are fed the same pre-EQ
  mono-summed samples in `processBlock`, and both `update()` every timer tick. `EQCurveDisplay::
  analyzerDb(freqLo, freqHi)` picks whichever instance covers a pixel's frequency, smoothstep-crossfaded
  over 1 octave around a 250 Hz crossover so there's no seam. Internal arrays are `std::vector` (sized
  at construction) instead of fixed `std::array`s to allow the two different sizes.
- Hann window, 75% overlap (hop = fftSize/4) on each instance for fluid motion.
- **Threading — one lock-free hand-off per instance, no locks/alloc/blocking on the audio thread:**
  - AUDIO thread `pushBlock()` mono-sums into a `circularBuffer`; every `hopSize` samples it publishes
    the last `fftSize` samples (time-ordered) into shared `fftData` and sets `atomic<bool>
    nextFFTBlockReady`. Gated: only writes when the flag is clear (drops a window otherwise).
  - MESSAGE thread `update()` (called from `EQCurveDisplay`'s 30 Hz timer, for BOTH instances) copies
    `fftData` out, clears the flag, then does the FFT + all smoothing. Everything the display reads is
    message-thread-only. `fftData` + the one atomic are the ONLY cross-thread state (verified race-free).
- **Smoothing pipeline:** rawDb -> `applyOctaveSmoothing()` (1/24-octave, power-averaged) -> per-bin
  time smoothing. The octave-smoothing minimum-average floor is `minAverageBandwidthHz` (currently
  **150 Hz** — an absolute Hz bandwidth shared by both instances, NOT a fixed bin count; a fixed bin
  count let the finer-resolution slow instance reveal narrow notches the fast instance smooths away,
  which showed up as a visible dip right in the crossfade zone — fixed by sharing one Hz floor instead).
  Time smoothing is deliberately asymmetric IN DOMAIN: **attack = exponential in dB** (50 ms);
  **release = straight-line decay in linear amplitude** at a per-bin rate captured from each peak's own
  level (`releaseTimeToZeroSeconds` = **0.8s**, the reference for a full-scale peak; quieter peaks decay
  proportionally slower). This linear-amplitude release is what gives Pro-Q's "eases down then falls
  away" look on a dB scale. The release additionally caps the per-frame dB drop so the tail doesn't
  plunge-then-hard-clamp at the floor — that cap is derived PER BIN from each bin's own captured rate,
  NOT a shared/global constant (a global cap was tried and made every bin's terminal glide toward the
  floor look identical — exactly wrong right at transport-stop, when every bin decays via this branch at
  once; per-bin derivation keeps loud-peaked bins audibly outlasting quiet ones, matching Pro-Q).
- **Silence handling:** if no audio for `starvationMs` (150), drive targets to floor — same per-bin
  branch as normal release, no separate "stop" logic, so bins settle independently rather than all at once.
- **Display tilt:** `EQCurveDisplay::analyzerDb()` applies a cosmetic +3 dB/octave slope
  (`kAnalyzerTiltDbPerOctave`, pivoted at 1 kHz) purely to the displayed value — matches the convention
  most reference analyzers (incl. Pro-Q) use to counter real music's natural bass-heavy raw-energy tilt,
  so typical program material reads roughly flat/even. Zero effect on DSP/audio. The tilt FADES OUT
  smoothly over the last `kTiltFadeRangeDb` (10 dB) above the floor rather than cutting off abruptly —
  a hard cutoff caused a visible snap as bins (especially highs, where the tilt is biggest) faded toward
  silence; the fade keeps true silence flat at every frequency with no discontinuity in the approach.
- Display: sampled per-pixel via the same `xToFreq()` as the curve (log-axis alignment free), drawn as
  a curved (quadratic) path so peaks are rounded. Its dBFS→Y mapping is INDEPENDENT of the EQ gain grid.

## Pre/Post analyzer overlay (always on, no mode toggle)

- **POST-EQ mirrors PRE exactly:** `EQAudioProcessor` owns `analyzerPost` (order 12) + `analyzerPostLow`
  (order 13), fed the same pre-EQ fast/slow pair's architecture but tapped at the very end of
  `processBlock` (after every band AND the output trim). `EQCurveDisplay::blendFastSlow()` (free function,
  shared) does the identical crossfade blend for both pairs, so pre and post are apples-to-apples at
  every frequency — a single-instance post was tried first and made the two curves genuinely disagree in
  the low end even with NO EQ shaping there, since they were different-resolution measurements; fixed by
  giving post the full dual-FFT mirror.
- **Pre+Post is the ONLY mode now** — the earlier Pre/Post/Pre+Post/Off dropdown (`AnalyzerMode` enum,
  `getAnalyzerModeChoices`/`setAnalyzerModeIndex`, `analyzerModeBox`) was built, then removed entirely at
  the user's request. Pre always draws; post only draws once at least one band is `on` (with every band
  off, pre and post are identical, so a second curve would just be visual noise).
- **Post curve is Q-windowed, not full-width:** it only reveals itself near an ACTIVE band's own
  frequency, sized by that band's Q via the standard parametric-EQ octave-bandwidth formula
  (`BW = 2·asinh(1/(2Q))/ln(2)`), tapering via smoothstep to exactly zero at ±half that bandwidth.
  Multiple bands' windows combine via MAX (not sum) so overlapping bands blend and separate ones each
  reveal their own patch.
- **Both POSITION and COLOUR are blended by that window, not just visibility:**
  - Position: `displayedY = lerp(preY, postY, windowAlpha)` — post's PLOTTED position is pulled toward
    pre's, so outside a band's window it's pixel-identical to pre (genuinely coincident), not just faint.
    (A pure opacity-fade was tried first and left a floating "ghost" line, since post's true reading is
    never exactly identical to pre even in an unaffected region — two independent live measurements.)
  - Colour/alpha: also scaled continuously by `windowAlpha`, fading between `preContourAlpha` (0.28, NOT
    zero) and full weight — fading to zero-alpha caused a visible dip in combined boldness right at a
    window's edge, since at that point you go from "two overlapping lines' weight" to "just pre's own
    thinner line." Fading to pre's OWN alpha instead means the edge is indistinguishable from "pre is
    just there."
  - Drawn as per-pixel-column straight segments (not one continuous smooth path), specifically so
    segments below a small threshold can be skipped entirely — position/colour matching pre isn't enough
    on its own; stroking on top anyway stacks a second semi-transparent line and reads as "the whole
    line is back," darker than pre alone.
- **Difference RIBBON fill, monochrome, split by direction:** a filled shape between the pre curve and
  the (blended) post curve, built per-pixel-column as a small quad (`preYs[i..i+1]` vs the blended
  `pts[i..i+1]`) so each slice can pick its own colour by the LOCAL sign of `(post - pre)` — necessary
  since one band's boost can sit right next to another's cut. Iterated through several looks before
  landing here: grey → orange (rejected, broke the monochrome rule) → orange/maroon by direction
  (rejected) → grey/maroon → **final state: `boostColour = juce::Colours::white` (fully opaque, no
  tint), `cutColour = juce::Colours::white.withAlpha(0.75f)`** — both white-based now, distinguished
  only by how strongly each "erases" the grey underneath. These two constants live right at the top of
  the `showPost` block in `EQCurveDisplay.cpp`'s `paint()` and were tuned live many times in a single
  session — treat them as the most likely thing to get re-tuned again before anything else here.
- Every step across this whole feature was rebuilt + `auval`-validated before reporting done; user tested
  live in FL Studio after nearly every change (screenshots), so this was a genuinely iterative, mostly
  cosmetic-tuning feature once the core dual-analyzer architecture was in place.

## Bottom control section — two rows (this session)

Window grew 720x460 -> 720x519 to fit this; the graph area itself is UNTOUCHED (same size as always),
all the extra height went to the control section below it.

- **Row 1, line 1:** "Band X" / "No band selected" label, alone, centred on its own line.
- **Row 1, line 2** (small gap below line 1): filter-type dropdown, freq/gain/Q readout, then three
  small icon buttons (mute, listen, delete) — all ONE tight centred horizontal group. A floating
  Pro-Q-style POPUP panel (hover/select a point to show freq/gain/Q + mute/listen/delete near the
  point) was fully built earlier this session, then explicitly deleted at the user's request ("I don't
  like it, maybe I'll come back to it later") — these same three controls were relocated into this
  always-visible row instead. `EQCurveDisplay::removeSelectedBand()` is the public hook the delete
  button calls.
- **Divider:** thin 25%-alpha black horizontal line between row 1 and row 2.
- **Row 2:** In/Out knobs (left, inset only 8px from the edge, was 50 — matches the right side's inset),
  bypass pill + dB-range dropdown grouped on the right.
- **Icons are exact Tabler Icons SVG paths** (github.com/tabler/tabler-icons, MIT), NOT hand-drawn
  `juce::Path` approximations — fetched once via WebFetch this session and embedded as literal SVG
  strings, parsed at runtime through `juce::Drawable::createFromSVG` (see `makeTablerIcon()` in
  `PluginEditor.cpp`). Hand-drawn attempts at "power" and "headphones" both had real geometry bugs
  (arc angle-direction mistakes — JUCE's arc angles are clockwise from 12 o'clock, easy to get backwards)
  that the exact SVG data sidesteps entirely. Mute = Tabler "power", listen = Tabler "headphones",
  delete = Tabler "x". Stroke width forced to 1.5 (Tabler's own default is 2) so it reads thin at icon
  size ~14-16px. Bypass's pill design predates this and was NEVER hand-drawn-approximated — kept as-is.
- **Dropdown fixes (typeBox/dbRangeBox), several rounds:** (1) `EQLookAndFeel::drawComboBox` overridden
  to replace JUCE's default outline-chevron arrow with a small solid filled triangle. (2) Box
  width/height shrunk to hug text tightly (was spacious/button-like). (3) Real bug: JUCE's
  `LookAndFeel_V4::positionComboBoxText` hardcodes `width - 30` for the text label regardless of what
  the arrow actually looks like — with the now-compact box widths this truncated text (e.g. "12 dB" ->
  "..."). Fixed with an `EQLookAndFeel::positionComboBoxText` override reserving only 16px. Widths
  themselves are computed from ACTUAL text metrics (`juce::GlyphArrangement::getStringWidth` — plain
  `Font::getStringWidth` no longer exists in this JUCE version) of the longest real option ("High
  Shelf" / "30 dB") plus a padding constant, bumped from +18 to +34 after even that still truncated
  slightly (likely `Label`'s own internal border eating a few more px than accounted for).
- **Bypass visual feedback:** while bypassed, the ENTIRE background (graph + control section +
  dropdowns' own hardcoded white fill) fades from white toward `Colours::darkgrey` over ~a few hundred
  ms, driven by one shared fade value (`PluginEditor::bypassDim`) POLLED each 30Hz timer tick from the
  real `"bypass"` APVTS parameter (not just the button's click handler — automation can also change it).
  Pushed into three places every tick: `EQCurveDisplay::setBackgroundDim()`, `PluginEditor::paint()`'s
  own fill, and `EQLookAndFeel::setDimAmount()` (the dropdowns' background is drawn via the shared
  LookAndFeel, so it needed its own hook — easy to forget a THIRD place needs the same value). The pill
  also gets a `juce::DropShadow` white glow when toggled on.
  - **Glow-clipping bug FOUND AND FIXED (next session after the above was written):** the glow was
    reading as flat white, not a radiating halo, because JUCE clips a component's painting to its own
    bounds by default and `PillToggle`'s bounds were EXACTLY the visual pill's size (40x20) — zero room
    for the blur to spread. Fixed by giving `PillToggle` bounds 20px bigger in each dimension than the
    drawn pill (`glowMargin = 10.0f` on every side), with `hitTest()` overridden so the enlarged bounds
    don't also enlarge the clickable area (clicks still only register on the actual pill shape).
  - **Also found:** three OTHER places in `EQCurveDisplay` were hardcoded to plain `Colours::white`
    meant to represent "erase to background" (unselected point fill, the pre/post diff-ribbon's boost/
    cut colours) -- these stayed stark white and stood out once the background actually dimmed, since
    only the flat background fill itself was tracking `backgroundDim`. Fixed with a new
    `EQCurveDisplay::currentBackgroundColour()` helper (white blended toward darkgrey by
    `backgroundDim`), used everywhere a "white == background" colour was previously hardcoded.
- **Muted-band visual marker:** a muted band's point still floats on the graph but the combined curve
  already excludes it (see DSP section). Originally tried a single dashed vertical "tether" from the
  point down/up to the curve's actual value at that frequency; user asked instead for a dashed preview
  of THAT BAND'S OWN response shape (its own bell/shelf curve evaluated alone, not summed with the
  others) drawn across the full width via `PathStrokeType::createDashedStroke` — shows the actual shape
  being suppressed, not just a single point-to-curve link.
- **Listen/audition "spotlight" overlay (Pro-Q-style), several rounds:** while a band's listen mode is
  active, two vertical marker lines show the actual bandpass slice being heard, using the SAME
  Q-to-octave-bandwidth formula already used for the post-analyzer's Q-derived reveal window (so these
  are the true filter edges, not an approximation). Iterated with the user through: dashed lines only ->
  added an 18%-alpha flat dim overlay on the regions OUTSIDE the two lines (dims everything already
  drawn there -- grid/analyzer/curve/points -- in one pass) -> added a `juce::ColourGradient` drop-shadow
  hugging just outside each line (peaks at 30% alpha right at the line, fades to 0 over 14px), giving
  the "elevated spotlight" depth effect Pro-Q has -> finally switched the two marker lines themselves
  from dashed to solid (still faint, 30% alpha). **Auto-disengage rules added:** listen mode now clears
  itself (and resyncs `listenButton`'s ring) if the listened-to band gets muted (polled each timer tick,
  same tick that already drives the bypass-dim fade) OR if the user places a brand-new band anywhere
  (`EQCurveDisplay::createBandAt()` now calls `proc.setListenBand(-1)` unconditionally) -- listen mode
  should never be left "stuck" active on a band that's no longer meaningfully being previewed.

## Bottom control section — later revisions (title removed, empty state, editable readout, frame, logo)

Several more rounds after the section above, in later sessions:

- **"Band X" title line removed entirely** (not just left blank) — the band index is a reused-slot
  index (nearest-frequency-to-click), not "the Nth band you created," so a label like "Band 6" for the
  very first band placed was actively confusing. Row 1's total height stays the same as before (the
  freed vertical space is just blank margin, single remaining line centres within it).
- **Row 1 fully HIDDEN when no band exists**, not just disabled/greyed: `selectionChanged()` calls
  `setVisible(false)` on every row-1 control (dropdown, three labels, three icon buttons) when
  `getSelectedBand() < 0`. Space stays reserved; it just reads as empty rather than showing placeholder
  controls with nothing to control.
- **Editable freq/gain/Q readout:** the combined single label was split into THREE separate
  `juce::Label`s (`freqLabel`/`gainLabel`/`qLabel`), each `setEditable(false, true, true)` (double-click
  to edit) with `setupEditableLabel()` wiring `onEditorShow`/`onTextChange` → `setBandParamFromEditor()`,
  which parses the typed text and writes through a proper `beginChangeGesture()`/
  `setValueNotifyingHost()`/`endChangeGesture()` sequence (same host-automation-safe path as every other
  control — these can't use a plain `SliderAttachment` since the typed text needs parsing/clamping
  first). Freq accepts an optional trailing "k" shorthand (e.g. "2.5k" → 2500). Invalid input clamps to
  the parameter's range. `isBeingEdited()` guards `layoutReadoutLabels()`/the timer refresh from
  clobbering a label mid-edit.
  - **Spacing iteration (several rounds):** splitting the fixed width evenly, then sizing from
    worst-case text with `Justification::centred`, both looked wrong — centring a box that's wider than
    its own content pads BOTH sides, which is worse than the original single-label convention (one
    centred block with the gap embedded as literal characters). Also hit a real `Label` behaviour bug:
    JUCE squishes text horizontally to fit if the box is only a hair too narrow — fixed with
    `setBorderSize(BorderSize<int>(0))` + `setMinimumHorizontalScale(1.0f)`. Final approach: labels are
    `Justification::centredLeft`, the visual gap between values is embedded as literal trailing "  " in
    the `fs`/`gs` display strings built in `updateReadout()` (exactly replicating the original combined
    label's spacing), and `layoutReadoutLabels()` packs each label tightly against its OWN current text
    width (measured via `juce::GlyphArrangement::getStringWidth`) inside a fixed `readoutZone` reserved
    once in `resized()` from worst-case text ("20.00 kHz  -18.0 dB  Q 10.00") — so the dropdown/icons
    around the zone never move regardless of how tightly the three values happen to pack on any frame.
- **Row 1 layout — edges pinned, readout centred between (not row-centred):** at the user's request,
  `typeBox` is pinned to row 1's left edge (with a 20px inset) and the three icon buttons are pinned to
  the right edge (20px inset), rather than the earlier single tight cluster centred as one group.
  `readoutZone` is then centred in whatever space is LEFT between them (`controlsRow.
  withSizeKeepingCentre(readoutW, ...)`), not centred in the full row — this was the fix for an
  asymmetric-looking gap the user spotted (worst-case `readoutW` reservation left unused slack that
  silently padded the icon-side gap more than the dropdown-side gap when a fixed shared-group-centring
  approach was used instead).
- **Faint frame around row 1:** a 25%-alpha black rounded-rect (`row1PanelBounds`, computed in
  `resized()`, drawn in `paint()`) ties the dropdown/readout/icons together as one visual "band panel,"
  chosen by the user from four mockup options presented via the visualize tool. Two real bugs found
  fixing this:
  - **Frame appeared late / inconsistently after adding a band:** `setVisible()` on the row-1 children
    only invalidates each child's OWN rectangle, not the frame drawn on the editor itself outside those
    children's bounds — so the frame's border pixels didn't actually redraw until some unrelated repaint
    happened to sweep through (e.g. the timer tick), making it look like it "randomly" appeared later.
    Fixed with an explicit `repaint(row1PanelBounds)` call at the end of `selectionChanged()`.
  - **Frame's vertical edges clipped off / asymmetric:** the frame's horizontal expansion (originally 14,
    then 6) pushed its left/right edges past the plugin window's own edge — `controlsRow`'s left/right
    already sits exactly on the outer 8px window margin (same margin the graph above it uses), so ANY
    horizontal expansion beyond that pushes the vertical edges outside the visible window entirely,
    clipping them while the top/bottom edges (which fit) stay visible — explaining why only fragments
    near the corners showed up. Fixed by using `controlsRow.expanded(0, 8)` (zero horizontal expansion),
    so the frame's left/right edges line up exactly flush with the graph's border above it.
- **Row 2 fine-tuning:** Out knob nudged left/right and In knob nudged left by small pixel amounts at
  the user's direct request (net position now -7px on In, -15px on Out from their original spot); both
  knobs (and labels) nudged down 5px.
- **"AudioFlower" brand mark (`logoLabel`):** centred in row 2's leftover middle space between the
  knobs and the bypass/dB group, same "centre in whatever's left" approach as `readoutZone`. Set in
  Indie Flower (the company's brand font), embedded as a genuine binary resource (`Resources/
  IndieFlower-Regular.ttf`, added to `EQ.jucer`'s Resources group, `Projucer --resave` regenerates
  `BinaryData::IndieFlowerRegular_ttf`) rather than looked up by system font name, so it renders
  correctly on any machine, not just ones that happen to have the font installed.
  - **Real bug found:** `EQLookAndFeel::getLabelFont()` unconditionally forces EVERY label to Times New
    Roman, because JUCE's `LookAndFeel_V2::drawLabel()` always renders via the LookAndFeel's
    `getLabelFont()`, never `label.getFont()` directly — so a plain `label.setFont(...)` call is silently
    ignored plugin-wide (only the HEIGHT survives, since the override reads `label.getFont().getHeight()`
    before re-wrapping it in Times New Roman). Fixed by tagging `logoLabel` with
    `setComponentID("logoLabel")` and exempting that specific ID in the override (`if
    (label.getComponentID() == "logoLabel") return label.getFont();`) rather than forcing Times on it
    like every other label.

## UI style

- **Black and white only** (one intentional light-grey for the analyzer fill and knob body). Pure white
  graph background with a subtle static "grain" texture (generated once, shared by graph + editor bg).
- `EQLookAndFeel` (a `LookAndFeel_V4`, scoped to the editor instance, cleared in dtor):
  - Monochrome `ColourScheme` (reaches the ComboBox popup menus, which per-widget setColour can't).
  - All text = **Times New Roman** (via `getLabelFont`/`getComboBoxFont`/`getPopupMenuFont` + a shared
    static `timesFont()` for hand-drawn text; `drawPopupMenuItem` overridden to centre items).
  - **Custom rotary knobs** (`drawRotarySlider`): thin outline circle, short rim tick, value drawn as
    two lines (number over unit) INSIDE the circle. No fill/gradient.
- Graph: grid + curve run edge-to-edge; axis labels drawn bare on top (no backdrop). Freq labels
  20/50/100/200/500/1k/2k/5k/10k/20k; dB labels track the range dropdown (6/12/30 dB view presets,
  default 12; view-only, NOT persisted, NOT an APVTS param). Points: selected = black fill,
  unselected = white fill + black ring.

## Session-62 audit findings

Verdict: functionally clean (warning-free, auval-passing, analyzer hand-off verified race-free).
Fixed this session: removed dead `EQBand::isActive()`; corrected several stale comments (analyzer
attack "linear amplitude"→dB, release "fixed rate"→per-bin proportional, octave "1/6"→"1/24", and
`EQCurveDisplay` axis-label "backdrop" comments — the backdrop was removed).

Left as-is deliberately (noted, not bugs):
- `EQCurveDisplay::vMargin == 0.0f` — one term is now a no-op; kept as a named constant for future use
  and it still parameterises the vertical-gridline extent.
- Analyzer `sampleRate` (plain `double`) is written in `prepare()` and read on the message thread
  without an atomic — benign theoretical race (aligned double access is effectively atomic here, value
  is stable).

## Analyzer detail/smoothing retune, and top bar (undo/redo/presets), this session

- **Analyzer "blocky/flat" complaint, several rounds:** user compared against Pro-Q and said the
  analyzer curve looked too geometric/faceted with too few visible peaks. Iterated in BOTH directions
  before landing right: first tried WIDENING smoothing (1/24→1/12 octave, `minAverageBandwidthHz`
  150→250 Hz) and coarsening the curve's render spline (per-pixel quadratics → a Catmull-Rom spline
  sampled every 4px) to fix "blocky" — user said it looked *worse* (flatter, fewer peaks), the opposite
  of what they wanted. Corrected by going narrower than the ORIGINAL values instead: octave smoothing to
  1/32 (tighter than the original 1/24), spline anchor spacing tightened to 1.5px (tighter than even the
  original per-pixel version's effective resolution) -- user confirmed "alot better." Then found and
  fixed a real remaining bug: `minAverageBandwidthHz` (still 100 Hz at that point) was the dominant term
  almost everywhere below ~4.6 kHz once the octave-proportional formula got this narrow (its own fixed Hz
  width easily exceeded the octave math down there), producing a flat plateau around 400 Hz specifically
  -- narrowed to 15 Hz, now only matters below ~700 Hz. **Current values: `octaveSmoothingFraction` =
  1/32, `minAverageBandwidthHz` = 15.0f (both `SpectrumAnalyzer.h`), spline `step` = 1.5f
  (`EQCurveDisplay.cpp`)** -- if detail is ever complained about again, these three are the first place
  to look, in that order.
- **A cosmetic "2x more dramatic" amplitude-scaling idea was proposed, then explicitly declined by the
  user before any code was touched** -- not implemented, not a bug, just noting it was considered and
  rejected in case it comes up again.
- **Analyzer "shadow near the Hz labels" bug, found via a real debugging thread (not a guess):** turned
  out to be the analyzer curve's own `dbToY()` mapping the floor (-100 dBFS) to EXACTLY `y == height`,
  the same row the frequency axis labels sit in -- whenever a band sat somewhere quiet, the curve (and
  the post-EQ reveal window's bolder line specifically, whose WIDTH really is Q-derived, matching the
  user's own "same width as Q" observation) dipped into that row. **A bottom-margin fix was tried, user
  said it made things worse, and it was reverted** -- this is still unresolved/open if it comes up again;
  the diagnosis (proven correct) is documented here so it doesn't need re-deriving.
- **New top bar (window grew 720x539 → same width, +20px height for the bar) with undo/redo + a full
  preset system**, built in the planned order (layout → undo/redo → presets → save):
  - **Undo/redo**: a `juce::UndoManager` added to `EQAudioProcessor` (declared BEFORE `apvts` since it's
    passed into that constructor by reference), threaded through the whole plugin. Every existing
    `ButtonAttachment`/`ComboBoxAttachment`/`SliderAttachment`-driven control got undo support entirely
    for free -- JUCE's own `ParameterAttachment::beginGesture()` already calls
    `undoManager->beginNewTransaction()` internally on every gesture, confirmed by reading the JUCE
    source rather than assuming. The manual (non-attachment) writes -- graph drag/scroll, typed-value
    commits, band delete -- bypass that class entirely, so each needed an explicit
    `EQAudioProcessor::beginNewTransaction()` call added at the same point they already call
    `beginChangeGesture()` (see `EQCurveDisplay::beginGesture()`/`removeBand()` and
    `PluginEditor::setBandParamFromEditor()`), so one drag/edit/delete still collapses into one undo
    step rather than fragmenting per intermediate value.
  - **Presets**: whole-state save/load reusing the EXACT serialisation the plugin already had for DAW
    session recall (`apvts.copyState()`/`replaceState()` + XML), stored at `~/Music/Audio/Presets/
    AudioFlower/EQ/*.xml` (per-user, per-machine, plain files, nothing shared/synced). Four factory
    presets (`Init`, `Low Cut`, `Vocal Presence`, `Air Boost`) are auto-created once if the folder's
    missing them (`EQAudioProcessor::ensureFactoryPresetsExist()`, called from the constructor) -- built
    by ACTUALLY setting parameters through the normal normalised-value API and capturing the resulting
    state, not hand-written XML, since the freq range's skew makes hand-computing a correct normalised
    value error-prone; never overwrites an existing file, so a user's own re-save of a factory preset
    survives. The editor does NOT auto-load a preset at construction (would silently wipe out whatever
    the DAW session/live edits already had) -- starts on a neutral "Default" label until the user
    actually browses via prev/next arrows or clicking the name (opens a full `PopupMenu` list). Loading a
    preset clears undo history (same as `apvts.replaceState()` already does for session recall).
  - **Delete**: right-clicking a preset in that popup menu (a custom `PopupMenu::CustomComponent`-based
    `PresetMenuItem`, not a plain `addItem()` row, specifically so left vs. right click could be told
    apart) dismisses the menu and shows a confirm dialog before deleting the file.
  - **Save**: a floppy-disk icon opens an `AlertWindow` with a text field (prefilled with the current
    name) to save/overwrite under any name.
- **Styling every one of these dialogs/menus to match the plugin's monochrome/Times New Roman look took
  several real fixes, not just "set a colour":**
  - `AlertWindow`s (save + delete-confirm) don't inherit the editor's `LookAndFeel` automatically (not a
    child of the editor's component tree) -- needed `aw->setLookAndFeel(&lookAndFeel)` explicitly, plus
    new `EQLookAndFeel::getAlertWindowTitleFont/getAlertWindowMessageFont/getAlertWindowFont/
    getTextButtonFont` overrides (all just delegating to `timesFont()`) and manual monochrome colour
    overrides on the `AlertWindow`/`TextEditor`/`TextButton` colour IDs.
  - Matching the preset `PopupMenu` to the type/dB-range `ComboBox` dropdowns' look took THREE separate
    fixes, each confirmed via reading JUCE's actual call sites rather than guessing: (1) added
    `EQLookAndFeel::drawPopupMenuBackground` (white fill + black border) -- JUCE's V4 default is a
    near-invisible soft shadow with no crisp outline. (2) Added `getIdealPopupMenuItemSize` to force
    every plain-text dropdown item to the SAME 22px row height the hand-built `PresetMenuItem` already
    used -- confirmed via `juce_PopupMenu.cpp` that the actual call site is
    `getIdealPopupMenuItemSizeWithOptions`, which `LookAndFeel_V2`'s default implementation delegates to
    the plain method we override, so this virtual-dispatches correctly. (3) **The real remaining
    mismatch**, only found by checking actual numbers in code (not eyeballing screenshots, which weren't
    conclusive either way): `getPopupMenuFont()` was deriving its height from JUCE's own default popup
    font (17px) instead of a fixed size, while `PresetMenuItem::paint()` hardcoded 14px directly --
    fixed by hardcoding `getPopupMenuFont()` to 14px too, so both now match exactly.
  - Also unified the preset menu's row-hover highlight from a subtle 8%-alpha overlay to the SAME solid
    black-fill/white-text inversion the dropdowns already use (driven by the monochrome `ColourScheme`).
  - Centring the preset popup under the "Default" name label needed a real fix, not just
    `withTargetComponent`: JUCE aligns a popup's LEFT edge to its target area's left edge, so a
    zero-width target point doesn't centre anything, it just anchors that same left edge to a point.
    Fixed by building a target screen area at the SAME width the menu will actually end up (mirroring
    `PresetMenuItem::getIdealSize()`'s own width calc) centred on the label -- aligning two equal-width
    rects' left edges is equivalent to centring them on each other.
- **Recurring workflow note reinforced again this session:** FL Studio does not hot-reload -- several
  "it still looks the same" reports during this session turned out to be the user testing against a
  stale cached plugin instance, not an actual code problem. Worth proactively reminding about
  remove+re-add after a rebuild whenever a change doesn't seem to show up.

## Dropdown outline/arrow polish, row-1 fade-in, and a real "frame won't disappear" bug fix

- **Removed the visible outline box from `typeBox`/`dbRangeBox`** (`EQLookAndFeel::drawComboBox` no
  longer calls `g.drawRect` for the outline) at the user's request — kept the background fill (still
  blends with `dimAmount` while bypassed) and the click-to-open behaviour; only the border went away.
- **Dropdown arrow swapped from a solid filled triangle to an outline chevron** (Tabler Icons
  "chevron-down" proportions, 12:6 width:height, stroked not filled) after the user pointed out it was
  the one solid-fill glyph among an otherwise all-outline icon set (mute/listen/delete, undo/redo, save,
  preset prev/next are all outline-stroke Tabler icons already).
- **Row 1 fade-in, added then a real bug found/fixed while working on it:** row 1's controls (dropdown/
  readout/icons) and its surrounding frame now fade in together over a few frames the first time a band
  is placed (or right after the previously-selected band is deleted) — driven by a new `rowFadeAlpha`
  float, same polling pattern as `bypassDim`, applied as each control's own `setAlpha()` and also scaling
  the frame's stroke alpha in `paint()`. Only fades on the HIDDEN→visible transition; switching selection
  between two already-visible bands doesn't re-trigger it.
  - **While testing this, found a real, separate, pre-existing bug**: the row-1 frame could get
    permanently stuck visible after the FIRST band was ever placed — even once every band was deleted
    and every row-1 CONTROL had correctly gone invisible, the empty frame box remained forever. Diagnosis
    took several iterations: first suspected `selectedBand` going stale from undo/redo bypassing
    `removeBand()`'s cleanup (added a defensive self-heal in `EQCurveDisplay::timerCallback()` checking
    `bandOn(selectedBand)` each tick -- real, worth keeping, but NOT the actual bug here). Second
    suspected `typeBox.isVisible()` diverging from reality -- replaced it with a dedicated `row1Visible`
    bool set directly by `selectionChanged()` and read directly by `paint()`, removing all indirection --
    user confirmed content correctly went invisible but the frame STILL didn't disappear, proving the
    boolean logic itself was already correct and the real bug was in redraw/invalidation, not state.
    **Actual fix**: the targeted `repaint(row1PanelBounds)` at the end of `selectionChanged()` (added
    several sessions ago to make the frame appear promptly) was NOT reliably clearing the frame's stale
    border pixels on the reverse (visible→hidden) transition -- replaced with a full unscoped `repaint()`
    call, confirmed fixed by the user. Lesson: a narrow `repaint(rect)` isn't always trustworthy for
    "erase something that was drawn in that rect" -- a full repaint is the safe fallback when a targeted
    one behaves inconsistently, especially for something drawn directly in the parent's own `paint()`
    rather than by a child component.
- **Attempted to test this interactively via computer-use (Standalone app) rather than only asking the
  user to check** -- blocked because Accessibility/Screen Recording permissions for the Claude desktop
  app aren't granted on this machine. If the user wants live in-app verification in a future session, they
  need to grant those in the Claude desktop app's system settings first.

## Deferred / next-up features

- Bypass pill glow, the listen/audition spotlight overlay, and the muted-band shape preview are all DONE
  now (see "Bottom control section" above) — several rounds of live user feedback each, all resolved.
- Dual-FFT low end AND the post-EQ pre/post overlay are both DONE (see analyzer sections above).
  Detail/smoothing was retuned again this session (see "Analyzer detail/smoothing retune" above) --
  current values `octaveSmoothingFraction`=1/32, `minAverageBandwidthHz`=15.0f, spline `step`=1.5f, user
  confirmed "good checkpoint." The "shadow near the Hz labels" bug is now FIXED: real cause was the
  post-EQ curve drawing its own second stroke on top of pre's whenever a band's Q-window was active,
  regardless of whether pre and post actually differed — with no audio playing they're identical, so the
  second stroke stacked exactly on pre's own line and read as a shadow shaped like the band's Q window.
  Fixed by gating that second stroke on real pixel divergence between pre/post (`postYs` array +
  `realDiff` factor in the segment-stroke loop, `EQCurveDisplay.cpp`), skipped outright below ~2px of
  real difference instead of just fading its alpha down to pre's own weight (which still double-darkened
  the same pixels). An earlier 2px-clip-off-the-bottom attempt was tried first, confirmed via build+auval
  but the user reported no visible change, and was cleanly reverted before the real fix above.
- Known-but-unaddressed subtlety: neighbouring low-freq bins' independent per-bin release timers can
  drift out of sync (spatial raggedness during a fade-out) — separate from the temporal per-bin-rate
  fix already done; not yet tackled.
- Top bar (undo/redo + presets) is DONE this session (see above) — layout, undo/redo, load/browse/save/
  delete, and matching the preset menu's style to the existing dropdowns all confirmed working by the
  user. Natural next steps if requested: preset rename, an overwrite-confirmation prompt when saving
  over an existing name (currently silent), or detecting when live edits have drifted away from the
  loaded preset (currently the name label just always shows whichever preset was last explicitly
  loaded/saved, with no "modified" indicator).
- The floating Pro-Q-style popup (hover/select a point to show a panel near it) was fully built, then
  deliberately deleted at the user's request — they may want it back in some form later. Its mute/
  delete controls now live permanently in row 1 of the bottom section instead (see above).
- Bottom control section: title removed, empty-state hiding, editable freq/gain/Q readout, row-1 edge
  pinning + centred readout, the faint row-1 frame, and the "AudioFlower" Indie Flower brand mark are
  all DONE (see "Bottom control section — later revisions" above) — user paused here satisfied with the
  current state as of this session. Row 2's In/Out knob positions were nudged by small pixel amounts at
  the user's direct request; likely more of this kind of micro-adjustment if they return to it.
- If the plugin is ever distributed/shared beyond this machine, double check `Resources/
  IndieFlower-Regular.ttf` actually made it into the embedded binary on whatever machine builds it (it's
  registered in `EQ.jucer`'s Resources group and regenerated via `Projucer --resave`, not hand-written).
- An ambient full-window animated background ("video" layer, sourced from a real mp4) was designed, built,
  tuned (opacity), and rescoped to graph-only across a full session — then explicitly reversed and REMOVED
  entirely at the user's request. No trace of it remains in the source (verified via grep before the final
  rebuild); the generated frame folder was deleted too. Do not re-suggest this feature unprompted.
- AudioFlower brand/style system (LookAndFeel + icon module + a visual reference sheet) is DONE, built at
  the user's own explicit request, living OUTSIDE this project at `~/Documents/AudioFlower-Brand/SharedUI/`
  (`AudioFlowerLookAndFeel.h/.cpp`, `AudioFlowerIcons.h`) — a logic-identical copy of this plugin's own
  `EQLookAndFeel`, meant to be dropped into a future plugin's `Source/` folder. EQ itself was NOT changed to
  use it; EQ still has its own local `EQLookAndFeel`. The two are currently byte-for-byte equivalent in
  logic but will drift apart the moment either one is edited without also updating the other — no automatic
  sync between them.

## Persistent project memory

Detailed session-by-session history lives in the auto-memory `project_eq.md` (indexed in `MEMORY.md`).
This CLAUDE.md is the condensed current-state snapshot; the memory file has the full "why" behind each
decision if deeper context is needed.
