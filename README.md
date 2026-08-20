# EQ — a parametric equalizer plugin

An AU/VST3 audio plugin built for real mixing work, not as a demo. Six bands,
per-band dynamic EQ, live spectrum analysis, and an interface designed around
how you actually reach for an EQ mid-session.

https://github.com/user-attachments/assets/a3f4d9ef-b67a-4938-9f36-5335dceb4ec8

## How this was built

I'm an audio engineer and production manager, not a software engineer. I built
this using AI-assisted development — I directed every product, interface, and
sound decision, and evaluated every result the way I evaluate any studio work:
by ear and by eye, in a DAW, on real material.

That means the interesting part of this project isn't the code. It's the
hundreds of judgment calls that got it from "working" to "usable," which is
the gap most tools never close.

## Decisions I made and why

**Gain range is ±18 dB, not ±30.** A single band pushed harder than that is
almost always the wrong tool — a notch or a pass filter is what you actually
want, and the plugin has both. The narrower range keeps it precise.

**The dynamic EQ has no threshold control.** Most dynamic EQs hand you
threshold, attack, and release per band — three more things to dial in before
you hear anything useful. This one asks for one number: how far the band should
move. The detector adapts to each band's own recent level, so it reacts to
*changes* in the material rather than compressing continuously. Tuning that
behavior was the longest part of the project, because "correct" and "feels
right on a vocal" aren't the same target.

**Every control lives on the graph.** No bottom panel, no tabs. Hover a band
and its controls appear where that band is. The reasoning: in a mix you're
already looking at the curve — making you travel to a separate panel to change
a filter type breaks the loop between hearing and adjusting.

**Things I built and then deleted:** an ambient video background, an output
meter, a skin system, an "automation mode," and a window-linked zoom feature.
Each one worked. Each one made the plugin worse to use, so it's gone. Knowing
what to cut mattered more than knowing what to add.

## What it does

| | |
|---|---|
| Bands | 6 — Bell, Low Shelf, High Shelf, High-Pass, Low-Pass, Notch |
| Slope | 12 / 24 / 36 / 48 dB/oct on pass filters |
| Gain | ±18 dB per band |
| Q | 0.1 – 10 |
| Dynamics | Per-band compression or expansion, ±18 dB, automatic threshold |
| Analysis | Live pre/post spectrum, with the post curve revealing only where a band is actually working |
| Formats | AU, VST3, standalone — macOS |

Four factory presets ship embedded in the plugin itself, so a fresh install
always has a working preset list.

## Verified, not assumed

The dynamic EQ is checked by an automated test that renders audio through the
actual compiled plugin — a scripted signal in, measured gain out — rather than
trusting that it sounds right. Compression, expansion, and a control case all
measure within 0.05 dB of the programmed value.

I also tested every build by hand in FL Studio, which is where most of the
design decisions above actually came from.

## Building it

Requires [JUCE 8](https://juce.com/get-juce/) and Xcode. Open `EQ/EQ.jucer` in
Projucer, then build the `EQ - All` target from
`EQ/Builds/MacOSX/EQ.xcodeproj`.

## Fonts

The interface uses IBM Plex Mono (numerics and labels) and Indie Flower (the
wordmark), both under the SIL Open Font License. License texts are included
alongside the fonts in `EQ/Resources/`.

## Status

Feature-complete and stable. Part of a small suite of audio plugins built under
the name AudioFlower.
