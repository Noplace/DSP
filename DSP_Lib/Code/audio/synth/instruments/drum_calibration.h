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

// Drum calibration: recorded hits of real drums decomposed into the drum's ringing modes and a
// residual (commuted synthesis, as the piano's excitations).
//
// Each hit is analysed into
//  - modes: decaying sinusoids fitted to the ringing (frequency, T60 and the complex amplitude at
//    the hit). The kit plays them on its modal resonators, so a re-strike adds to the ringing
//    vibration and a choke (closed hi-hat, muted conga) damps it.
//  - residual: the recording minus those modes over its first few hundred ms. This is everything
//    that isn't a clean ringing mode: the stick or beater contact, the snare wires, the noisy
//    wash of a cymbal, the room. It is played back as recorded and fades out at its end.
// Playing the residual and starting the modes from the same instant rebuilds the recording; after
// the residual has faded only the modes ring on.

struct DrumCalMode {
  float freq_hz = 0;
  float t60_s = 0;
  float re = 0, im = 0;  // phasor at the hit: output sample j = Im((re + i im) p^(j+1)), p = r e^(iw)
};

struct DrumCalTake {
  float velocity = 1;        // 0..1, the MIDI velocity this take stands for (from its level)
  float level = 0;           // loudest 50 ms RMS of the recording, after the note's gain
  float residual_scale = 0;  // residual sample = int16 value * residual_scale
  std::vector<int16_t> residual;
  std::vector<DrumCalMode> modes;
  std::string source;        // file name of the recording
};

struct DrumCalNote {
  int midi_note = 0;
  int from_note = -1;        // >= 0: plays from_note's takes transposed by `semitones`
  float semitones = 0;
  float gain = 1;            // level of a derived note vs its source
  uint32_t rate = 0;         // sample rate of the takes
  std::vector<DrumCalTake> takes;  // sorted by velocity; similar velocities are round robins
};

struct DrumCalibration {
  std::string name;
  std::string path;          // file it was loaded from (set by Load)
  std::vector<DrumCalNote> notes;

  bool Save(const char* path) const;
  bool Load(const char* path);
  const DrumCalNote* Find(int midi_note) const;
  // The note whose takes `midi_note` plays (itself, or its source for a derived note).
  const DrumCalNote* Source(int midi_note, float* semitones, float* gain) const;
};

}
}
}
}
