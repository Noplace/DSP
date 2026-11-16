# Calibration data

Files here make the synth's physical models match real instruments. `MidiSynth` looks for this
folder next to or above the executable and uses any calibrations it finds; without them it falls
back to hand-set presets.

| File | Instrument | Source recordings | Licence |
|---|---|---|---|
| `harpsichord_english.cal` | GM programs 7-8 (harpsichord, clavinet) | VCSL "Harpsichord, English" (Zuckermann kit harpsichord), Normal stop, 28 sustained notes A#1-E6 | CC0 1.0 ([sgossner/VCSL](https://github.com/sgossner/VCSL)) |
| `piano_steinway_b.cal` | GM programs 1-4 (grands; honky-tonk = same with detuned unisons) | VCSL "Grand Piano, Steinway B", sustain pedal up, close mic, 42 notes A#0-G#7 x 3 velocity layers | CC0 1.0 ([sgossner/VCSL](https://github.com/sgossner/VCSL)) |
| `drums_vcsl.cal` | GM drum kit (channel 10): snare, side stick, kick 35, closed / open / pedal hi-hat | VCSL "Snare Drum, Modern 1", "Bass Drum 1", "Hi-Hat Cymbal" (see Drum kit below) | CC0 1.0 ([sgossner/VCSL](https://github.com/sgossner/VCSL)) |

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

## Drum kit (GM channel 10)

The kit (`instruments/drum_kit.*`) is physically modelled and works without calibration data.
Each piece (kick, snare, toms, hi-hat, cymbals, hand percussion) is one physical object, and
its GM notes are strokes on it: 42/44/46 on one hi-hat, 51/53 on one ride, 37/38/40 on one
snare.

- Membranes use Bessel modes with air loading and a velocity-dependent contact pulse.
  Tension-modulation pitch glide follows the head's energy.
- The snare wires are noise gated by the snare-side head.
- Cymbals are clouds of 170-230 modes with a noise-driven energy cascade.

`drums_vcsl.cal` holds `note parameter value` lines that override the hand-set parameters
(the names are the `kParams` table in `drum_kit.cpp`). `DSP_Render drumfit` fits them by
coordinate descent on a profile distance: band energies in 5 bands x 5 time windows after the
onset, the -20/-40 dB decay times and, for membranes, the pitch. Levels are not fitted; the kit
balance lines at the end of the file are set by hand.

| Note | Fitted to | Band difference before / after (dB, held-out take) |
|---|---|---|
| 38 snare | Snare Drum, Modern 1: snares on, layers v9 / v7 / v6 at velocity 120 / 80 / 45 | 6.3 / 3.3 (f), 7.5 / 3.8 (mp) |
| 37 side stick | Snare Drum, Modern 1: `stick` | 11.9 / 6.9 |
| 35 kick | Bass Drum 1: v7 / v5 at 120 / 85, without pitch (the low end is too noisy for it) | 7.0 / 5.4 |
| 42 closed hi-hat | Hi-Hat Cymbal: `HitC` v4 / v3 / v2 at 118 / 78 / 35 | 14.2 / 3.5 (f), 14.8 / 2.9 (p) |
| 46 open hi-hat | Hi-Hat Cymbal: `HitO` at 105 | 11.1 / 2.4 |
| 44 pedal hi-hat | Hi-Hat Cymbal: `Close` at 80 | 18.8 / 3.6 |

Velocities were chosen so that the synth's level steps match the recorded layers. The rest of
the kit stays hand-tuned:

- Toms: the VCSL tom folders only hold quiet rim strokes at the top level.
- Crash, ride, splash and china: VCSL has orchestral clash and suspended cymbals, not kit
  cymbals.
- Room: the recordings include room sound, so their tails (kick, snare) are longer than the
  dry model's.

The recordings are git-ignored, under `reference/vcsl-snare-modern-1`, `reference/vcsl-bass-drum-1`,
`reference/vcsl-hihat`, `reference/vcsl-tom-1` and `reference/vcsl-tom-2`. They come from
`Membranophones/Struck Membranophones/...` and `Idiophones/Struck Idiophones/Hi-Hat Cymbal`.

To refit a note, run `drumfit` with the parameters to fit and `recording@velocity` pairs. It
starts from the values already in the file and replaces only those lines:

```bash
build\x64\Release\DSP_Render.exe drumfit data\calibration\drums_vcsl.cal 42 peak_lo,peak_hi,t60_lo,t60_hi,damp reference\vcsl-hihat\HiHat_HitC_v4_rr1_Mid.wav@118 reference\vcsl-hihat\HiHat_HitC_v3_rr1_Mid.wav@78
```

Compare a drum note with a recording (band energies and centroid over time, side by side, plus
an A/B WAV):

```bash
build\x64\Release\DSP_Render.exe drumcompare 38 reference\vcsl-snare-modern-1\Snare2_HitSN_v9_rr2_Mid.wav build\samples\ab_snare.wav 120
```

Other tools:

- `drum <note> [velocity]`: profile and envelope of one note.
- `drums [rate] [dir] [velocity] [first] [last]`: level and decay table, with optional WAVs.
- `drumgroove <out.wav>`: a test performance.
- `drumcpu`: CPU cost per drum voice.
