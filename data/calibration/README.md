# Calibration data

Files here make the synth's physical models match real instruments. `MidiSynth` looks for this
folder next to or above the executable and uses any calibrations it finds; without them it falls
back to hand-set presets.

| File | Instrument | Source recordings | Licence |
|---|---|---|---|
| `harpsichord_english.cal` | GM programs 7-8 (harpsichord, clavinet) | VCSL "Harpsichord, English" (Zuckermann kit harpsichord), Normal stop, 28 sustained notes A#1-E6 | CC0 1.0 ([sgossner/VCSL](https://github.com/sgossner/VCSL)) |
| `piano_steinway_b.cal` | GM programs 1-4 (grands; honky-tonk = same with detuned unisons) | VCSL "Grand Piano, Steinway B", sustain pedal up, close mic, 42 notes A#0-G#7 x 3 velocity layers, plus the key-up (release) recordings of 33 notes A#0-D6 x 3 layers | CC0 1.0 ([sgossner/VCSL](https://github.com/sgossner/VCSL)) |
| `drums_vcsl.cal` | GM drum kit (channel 10): 41 of 61 notes from 125 recorded hits (modes + residual), built from `drums_vcsl.txt` | VCSL kick, snare, toms, hi-hat, suspended cymbals and hand percussion (see Drum kit below) | CC0 1.0 ([sgossner/VCSL](https://github.com/sgossner/VCSL)) |

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

## Drum kit (GM channel 10)

`drums_vcsl.cal` is binary, like the piano's (`DrumCalibration` in `instruments/drum_calibration.h`,
about 6 MB). It is built from 125 recorded hits and covers 41 of the kit's 61 notes. The other 20
are physically modelled (`instruments/drum_kit.cpp`): timbales, china, vibraslap, whistles,
cuica, surdo, bell tree, castanets and the GM2 extras 27-34.

### What the file holds

Each recorded hit is decomposed the way the piano is (commuted synthesis):

- **Modes:** up to 16-128 per hit (`max_modes` in the recipe). These are the drum's ringing
  partials. Each has its frequency, its T60 measured frame by frame, and its complex amplitude at
  the hit, all fitted jointly by least squares to the tail.
- **Residual:** the recording minus those modes, for its first 200-2500 ms, faded out at the end
  and stored as 16-bit. This is everything that is not a clean ringing mode: the stick or beater
  contact, the snare wires, the cymbal wash, the room.

The kit plays the residual and starts the modes on its resonators from the same sample, which
rebuilds the recorded hit. After the residual has faded, only the modes ring on. Because the modes
are live resonators:

- A re-strike adds to the vibration already ringing on the same drum.
- A closed or pedal hi-hat damps the open hat's modes, and so does a muted conga or triangle.
- Velocity scales a take smoothly between the recorded layers.
- Round robins alternate between takes, never the same one twice in a row.

### How notes are matched to recordings

The recipe `drums_vcsl.txt` (text, committed) lists the recordings for each note. Notes are
either recorded or derived:

- **Recorded:** 36 kick, 38 snare, 37 side stick, 39 clap, 48 and 43 toms, 42/44/46 hi-hat,
  49 and 57 crashes, 53 ride bell, and the hand percussion.
- **Derived:** played from a recorded note, transposed, on their own drum.
  - 35 is the kick 2 semitones down.
  - 40 is the snare 1.5 semitones up.
  - 50/47/45/41 come from the two recorded toms, within +-3 semitones.
  - 55 splash is crash 1 up 5 semitones.
  - 51/59 ride is crash 2's stick hits.
  - 77 is the wood block down 4 semitones.

The builder sets each take's velocity from its level, using the kit's loudness curve. It also
scales each note to the level the recipe gives it, which is the kit balance.

Some checks show the fit is sound. The fitted modes cannot ring louder than the recording later
in the tail: too-long decays are shortened window by window. Modes more than 50 dB below a take's
strongest are dropped. Below `min_hz` the builder doesn't look for modes, so room rumble stays in
the residual.

### Before and after

Synth versus recording, average band difference over 5 bands x 5 time windows (`drumcompare`):

| Note | Hand-set model | Calibrated |
|---|---|---|
| snare | 6.3 dB | 0.4 dB |
| kick | 7.0 dB | 0.7 dB |
| closed / open / pedal hi-hat | 14.2 / 11.1 / 18.8 dB | 0.1 / 0.0 / 2.2 dB |

The decay times now match the recordings: kick -40 dB at 0.78 s vs 0.69 s recorded, open hat
1.28 s vs 1.38 s. The calibrated numbers compare against the hits the file was built from. Where
the synth differs is after the residual has faded and between velocity layers.

### Recordings

The recordings are git-ignored and live under `reference/vcsl-*`. They come from VCSL (CC0):

- `Membranophones/Struck Membranophones/`: Bass Drum 1; Snare Drum, Modern 1; Tom 1/Stick;
  Tom 2/Stick; Conga; Bongos.
- `Idiophones/Struck Idiophones/`: Hi-Hat Cymbal; Suspended Cymbal 1 and 2 (stick and bell hits
  only); Tambourine 1; Cowbells; Agogo Bells; Claves; Woodblock; Claps; Cabasa; Shaker, Small;
  Sleigh Bells; Guiro; Triangles.

Only single hits were downloaded, no rolls, shakes or bowed files. The cymbals are orchestral
suspended cymbals played with sticks; VCSL has no kit crash or ride.

### Rebuilding and checking

Rebuild the calibration:

```bash
build\x64\Release\DSP_Render.exe drumcal data\calibration\drums_vcsl.txt data\calibration\drums_vcsl.cal
```

List what the file holds for a note:

```bash
build\x64\Release\DSP_Render.exe drumcalinfo data\calibration\drums_vcsl.cal 38
```

Compare a note with a recording and write an A/B WAV:

```bash
build\x64\Release\DSP_Render.exe drumcompare 38 reference\vcsl-snare-modern-1\Snare2_HitSN_v9_rr2_Mid.wav build\samples\ab_snare.wav 120
```

Show which calibrations the synth found:

```bash
build\x64\Release\DSP_Render.exe info
```

Other tools:

- `drum <note> [velocity]`: profile and envelope of one note.
- `drums [rate] [dir] [velocity] [first] [last]`: level and decay table, with optional WAVs.
- `drumgroove <out.wav>`: a test performance.
- `drumcpu`: CPU per drum voice.

The studio's channel inspector shows a CALIBRATED badge next to the instrument when it plays a
calibration file. The badge's tooltip names the file. Instruments without a calibration file
show PRESET.
