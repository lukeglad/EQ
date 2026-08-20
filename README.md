# EQ

**[audioflower.art](https://audioflower.art)**

A six-band parametric equalizer for macOS. Per-band dynamic EQ, live spectrum
analysis, and an interface where every control lives on the frequency graph
instead of a panel underneath it.

https://github.com/user-attachments/assets/f34c02cf-c43f-44cb-8ce0-d26f1b71cfcf

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

Four factory presets are embedded in the plugin itself, so a fresh install always
has a working preset list.

## Building it

Requires [JUCE 8](https://juce.com/get-juce/) and Xcode. Open `EQ/EQ.jucer` in
Projucer, then build the `EQ - All` target from `EQ/Builds/MacOSX/EQ.xcodeproj`.

## License

AGPLv3 — see [LICENSE](LICENSE). This is JUCE's open-source option, and it's
copyleft: anyone who builds on this and ships it has to publish their source too.
Copyright remains mine.

## Status

Feature-complete and stable. The first audio tool built under the name
[AudioFlower](https://audioflower.art).

Built with AI coding agents. Every product, user interface, and sound design
decision is made by me, tested in FL Studio on real material.
