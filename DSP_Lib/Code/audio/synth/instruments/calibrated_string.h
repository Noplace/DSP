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

#include <memory>
#include <vector>

namespace dsp {
namespace audio {
namespace synth {
namespace instruments {

// A plucked string calibrated from recordings of a real instrument (commuted waveguide synthesis):
//  - string loop per note from measured inharmonicity and decay (smoothed across the keyboard)
//  - excitation = the real pluck + soundboard response, recovered by inverse-filtering the
//    recording with that same loop, pitch-shifted to each note from the nearest recorded one
class CalibratedString : public InstrumentProcessor {
 public:
  CalibratedString(std::shared_ptr<const StringCalibration> calibration, real_t release_t60_s = 0.15f,
                   real_t level = 0.35f);
  ~CalibratedString();

  int Load() override;
  int Unload() override;
  InstrumentData* NewInstrumentData() override { return new InstrumentData(); }
  real_t Tick(int v) override;
  int SetFrequency(real_t freq, int v) override;
  int NoteOn(int v) override;
  int NoteOff(int v) override;
  void set_instrument_data(InstrumentData* idata) override { cdata_ = idata; }
  bool IsVoiceActive(int v) const override { return voices_ && voices_[v].active; }
  real_t VoiceLevel(int v) const override { return voices_ ? voices_[v].level : 0; }
  void BendFrequency(real_t freq, int v) override;

  // Smoothed parameters for a MIDI note (for tools/inspection).
  LoopParams NoteParams(int midi_note) const;

 private:
  struct Voice {
    StringLoop loop;
    const float* excitation;
    int exc_len, exc_pos;
    float dc_x1, dc_y1;
    float level;
    float base_gain;      // held per-period loss gain (restored on retrigger)
    int note;
    real_t freq;
    bool active, released;
  };

  void BuildTables();

  std::shared_ptr<const StringCalibration> cal_;
  real_t release_t60_s_, level_;
  float gain_;                                 // normalises the recordings' level
  LoopDesign designs_[128];
  LoopParams params_[128];
  std::vector<float> excitations_[128];        // per MIDI note, at the synth's sample rate
  Voice* voices_;
  float dc_coef_, level_decay_;
  InstrumentData* cdata_;
};

}
}
}
}
