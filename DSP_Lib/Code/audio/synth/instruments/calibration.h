/*****************************************************************************************************************
* Copyright (c) 2014 Khalid Ali Al-Kooheji                                                                       *
*                                                                                                                *
* Permission is hereby granted, free of charge, to any person obtaining a copy of this software and              *
* associated documentation files (the "Software"), to deal in the Software without restriction, including        *
* without limitation the rights to use, copy, modify, merge, publish, distribute, sublicense, and/or sell        *
* copies of the Software, and to permit persons to whom the Software is furnished to do so, subject to the       *
* following conditions:                                                                                          *
*                                                                                                                *
* The above copyright notice and this permission notice shall be included in all copies or substantial           *
* portions of the Software.                                                                                      *
*                                                                                                                *
* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT          *
* LIMITED TO THE WARRANTIES OF MERCHANTABILITY, * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.          *
* IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, * DAMAGES OR OTHER LIABILITY,      *
* WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE            *
* SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.                                                         *
*****************************************************************************************************************/
#pragma once

#include <stdint.h>
#include <string>
#include <vector>

namespace dsp {
namespace audio {
namespace synth {
namespace instruments {

// Measured decay of one partial (version 3). Low piano partials decay in two stages -- fast while
// the unison strings move together, then a long, quiet aftersound -- and each partial does it its
// own way, which no smooth loss filter can follow, so the instrument plays them as resonators.
struct PartialDecay {
  float t60_s = 0;          // decay of the partial (its first stage if it has two); 0 = not measured
  float after_t60_s = 0;    // aftersound decay (two-stage partials)
  float after_level = 0;    // aftersound level vs the first stage at the onset (0 = single stage)
};

// One velocity layer of a recorded note.
struct ExcitationLayer {
  float velocity = 1.0f;          // 0..1, the playing strength this layer represents
  float peak = 0;                 // peak level of that recording
  std::vector<float> samples;     // excitation at CalibratedNote::excitation_rate
  std::vector<PartialDecay> partials;  // version 3: partial n at index n - 1, at this velocity
  // Version 4: the key-up sound of a note played at this velocity (damper thump, key and hammer
  // return), from key-up on, with the string itself removed (the instrument plays its own string
  // through the damping). At CalibratedNote::excitation_rate; empty = not recorded.
  std::vector<float> release;
};

// Parameters measured from one recorded note of a real instrument.
struct CalibratedNote {
  int midi_note = 0;
  float f0_hz = 0;            // as recorded
  float inharmonicity = 0;    // B
  float b1 = 0, b3 = 0;       // decay model sigma(f) = b1 + b3 f^2
  float peak = 0;             // peak level of the recording (for level normalisation)
  uint32_t excitation_rate = 0;
  std::vector<float> excitation;  // pluck + body, from inverse-filtering the recording
  // Version 2 (piano): a second, slightly detuned string for the two-stage decay, and several
  // velocity layers (if present they replace `excitation`).
  float after_b1 = 0, after_b3 = 0;  // aftersound decay model (0 = no second string)
  float after_level = 0;             // linear share of the excitation fed to the second string
  float detune_cents = 0;            // second string vs the first
  std::vector<ExcitationLayer> layers;  // sorted by velocity
  // Version 3: per-partial decays (of `excitation`'s recording; layered notes have them per layer).
  // They replace the second string above, which calibrations with them leave off (after_level 0).
  std::vector<PartialDecay> partials;
  // Version 4: the dampers, measured from key-up recordings. After key-up the string rings on for
  // damper_delay_s, then decays with release_t60_s (0 = not measured: the instrument's default).
  float release_t60_s = 0;
  float damper_delay_s = 0;
  std::vector<float> release;  // key-up sound of `excitation`'s recording (layered notes: per layer)
};

// A calibrated string instrument: one entry per recorded note, sorted by MIDI note.
struct StringCalibration {
  std::string name;
  std::string path;  // file it was loaded from (set by Load)
  std::vector<CalibratedNote> notes;

  bool Save(const char* path) const;
  bool Load(const char* path);
};

}
}
}
}
