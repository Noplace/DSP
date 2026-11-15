# Calibration data

Files here make the synth's physical models match real instruments. `MidiSynth` looks for this
folder next to or above the executable and uses any calibrations it finds; without them it falls
back to hand-set presets.

| File | Instrument | Source recordings | Licence |
|---|---|---|---|
| `harpsichord_english.cal` | GM programs 7-8 (harpsichord, clavinet) | VCSL "Harpsichord, English" (Zuckermann kit harpsichord), Normal stop, 28 sustained notes A#1-E6 | CC0 1.0 ([sgossner/VCSL](https://github.com/sgossner/VCSL)) |
| `piano_steinway_b.cal` | GM programs 1-4 (grands; honky-tonk = same with detuned unisons) | VCSL "Grand Piano, Steinway B", sustain pedal up, close mic, 42 notes A#0-G#7 x 3 velocity layers | CC0 1.0 ([sgossner/VCSL](https://github.com/sgossner/VCSL)) |

Piano specifics: string stiffness is fitted with Abel-Smith allpass dispersion filters (partials
within ~1-3 cents of the recordings across the keyboard), every velocity layer has its own
excitation (crossfaded by velocity), and the piano's own stretched tuning is reproduced. The
partials below 4 kHz play as resonators with each partial's own measured decay, per velocity
layer. Each decays in two stages: a fast first stage, then the long, quiet aftersound of the
unison strings, which rings for 30-100 s on low bass partials. A crossover hands the rest of the
spectrum to the string loop.

## What is in a .cal file

Per recorded note: measured fundamental, inharmonicity B, decay model sigma(f) = b1 + b3 f^2, and the
excitation (the real pluck plus soundboard response) recovered by inverse-filtering the recording
with the fitted string loop. Version 3 files (the piano) also hold the decay of each of the first
24 partials per velocity layer: T60, and for two-stage partials the aftersound's T60 and level. The synth smooths the parameters across the keyboard and transposes
each excitation to the notes around it.

## Rebuilding

The recordings are not in the repository (see `reference/`, git-ignored). To rebuild:

```bash
build\x64\Release\DSP_Render.exe calibrate reference\vcsl-harpsichord-english\sustain data\calibration\harpsichord_english.cal
```

```bash
build\x64\Release\DSP_Render.exe calibrate reference\vcsl-piano-steinway-b\sustain data\calibration\piano_steinway_b.cal
```

Check a note against its recording (prints partial levels / decay side by side, writes an A/B WAV):

```bash
build\x64\Release\DSP_Render.exe compare 7 reference\vcsl-harpsichord-english\sustain\ZuckermannKitHarpsi_Normal_Sus_C3_rr1.wav build\samples\ab.wav
```

Note: VCSL names notes with middle C = "C3"; the tools detect the octave from the audio.
