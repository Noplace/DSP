# Calibration data

Files here make the synth's physical models match real instruments. `MidiSynth` looks for this
folder next to or above the executable and uses any calibrations it finds; without them it falls
back to hand-set presets.

| File | Instrument | Source recordings | Licence |
|---|---|---|---|
| `harpsichord_english.cal` | GM programs 7-8 (harpsichord, clavinet) | VCSL "Harpsichord, English" (Zuckermann kit harpsichord), Normal stop, 28 sustained notes A#1-E6 | CC0 1.0 ([sgossner/VCSL](https://github.com/sgossner/VCSL)) |
| `piano_steinway_b.cal` | GM programs 1-4 (grands; honky-tonk = same with detuned unisons) | VCSL "Grand Piano, Steinway B", sustain pedal up, close mic, 42 notes A#0-G#7 x 3 velocity layers, plus the key-up (release) recordings of 33 notes A#0-D6 x 3 layers | CC0 1.0 ([sgossner/VCSL](https://github.com/sgossner/VCSL)) |

Piano specifics: string stiffness is fitted with Abel-Smith allpass dispersion filters (partials
within ~1-3 cents of the recordings across the keyboard), every velocity layer has its own
excitation (crossfaded by velocity), and the piano's own stretched tuning is reproduced. The
partials below 4 kHz play as resonators with each partial's own measured decay, per velocity
layer. Each decays in two stages: a fast first stage, then the long, quiet aftersound of the
unison strings, which rings for 30-100 s on low bass partials. A crossover hands the rest of the
spectrum to the string loop.

Key-up: the release recordings start at key-up with the note still ringing. Calibration tracks
the string's partials through each one (heterodyned at each partial, smoothed over about a
period) and subtracts them. What is left is the key-up sound itself: the damper thump and the key
and hammer coming back. The partials' energy shows when the dampers touch (0-80 ms after key-up)
and how fast they stop the string (damper T60: about 1 s in the low bass, 0.4-0.7 s from the
tenor up). On key-up the synth waits that long, damps its own string (loop and resonators) with
that T60, and plays the key-up sound of the nearest recorded note, transposed and velocity
crossfaded like the excitations. Strings above the dampers (F6 up) and notes held by the sustain
pedal ring on without a key-up sound.

## What is in a .cal file

Per recorded note: measured fundamental, inharmonicity B, decay model sigma(f) = b1 + b3 f^2, and the
excitation (the real pluck plus soundboard response) recovered by inverse-filtering the recording
with the fitted string loop. Version 3 files (the piano) also hold the decay of each of the first
24 partials per velocity layer: T60, and for two-stage partials the aftersound's T60 and level.
Version 4 adds the dampers per note (T60 after key-up and the delay before they touch) and the
key-up sound per velocity layer (up to 0.4 s, string removed). The synth smooths the parameters
across the keyboard and transposes each excitation and key-up sound to the notes around it.
Older files (versions 1-3) still load; without key-up data the synth uses a generic damper decay.

## Rebuilding

The recordings are not in the repository (see `reference/`, git-ignored). To rebuild:

```bash
build\x64\Release\DSP_Render.exe calibrate reference\vcsl-harpsichord-english\sustain data\calibration\harpsichord_english.cal
```

```bash
build\x64\Release\DSP_Render.exe calibrate reference\vcsl-piano-steinway-b\sustain data\calibration\piano_steinway_b.cal
```

The key-up recordings are picked up from the sibling `release` folder (or give the folder as a
fifth argument: `calibrate <sustain dir> <out.cal> - <release dir>`; `-` instead turns them off).

Split one key-up recording into string and key-up sound (prints damper timing and decay, writes the
key-up sound):

```bash
build\x64\Release\DSP_Render.exe release reference\vcsl-piano-steinway-b\release\JHPiano_NoSusRel_Close_C4_vl4_rr1.wav reference\vcsl-piano-steinway-b\sustain\JHPiano_NoSus_Close_C4_vl4_rr1.wav build\samples\keyup_C4.wav
```

Check the synth's key-up against a recording (holds the note as long as the recorded one was
held, compares levels, damper timing and decay, writes recording-then-synth A/B):

```bash
build\x64\Release\DSP_Render.exe keyup 1 reference\vcsl-piano-steinway-b\release\JHPiano_NoSusRel_Close_C4_vl4_rr1.wav reference\vcsl-piano-steinway-b\sustain\JHPiano_NoSus_Close_C4_vl4_rr1.wav build\samples\ab_keyup_C4_vl4.wav
```

Check a note against its recording (prints partial levels / decay side by side, writes an A/B WAV):

```bash
build\x64\Release\DSP_Render.exe compare 7 reference\vcsl-harpsichord-english\sustain\ZuckermannKitHarpsi_Normal_Sus_C3_rr1.wav build\samples\ab.wav
```

Note: VCSL names notes with middle C = "C3"; the tools detect the octave from the audio.
