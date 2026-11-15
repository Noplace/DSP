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

// Parameters measured from one recorded note of a real instrument.
struct CalibratedNote {
  int midi_note = 0;
  float f0_hz = 0;            // as recorded
  float inharmonicity = 0;    // B
  float b1 = 0, b3 = 0;       // decay model sigma(f) = b1 + b3 f^2
  float peak = 0;             // peak level of the recording (for level normalisation)
  uint32_t excitation_rate = 0;
  std::vector<float> excitation;  // pluck + body, from inverse-filtering the recording
};

// A calibrated string instrument: one entry per recorded note, sorted by MIDI note.
struct StringCalibration {
  std::string name;
  std::vector<CalibratedNote> notes;

  bool Save(const char* path) const;
  bool Load(const char* path);
};

}
}
}
}
