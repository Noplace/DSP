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

namespace dsp {
namespace audio {
namespace synth {
namespace instruments {

// A resonance of the instrument body / soundboard (commuted into the excitation).
struct BodyMode {
  real_t freq_hz;
  real_t t60_s;   // ring time
  real_t gain;
};

struct WaveguideParams {
  // Decay of the fundamental while the key is held: T60 at middle C, scaled by (261.6/f)^slope.
  real_t t60_c4_s;
  real_t t60_slope;
  // Loop low-pass (0 = no high-frequency loss, 0.5 = strongest). Higher = darker, faster-dying
  // overtones. Reduced automatically for high notes.
  real_t damping;
  // Stiffness: allpass coefficient (0 = ideal string, negative = stretched partials like a
  // piano) and number of allpass stages.
  real_t stiffness;
  int stiffness_stages;
  // Excitation
  real_t pick_position;      // 0..0.5 of the string length (comb notches every 1/pos harmonics)
  real_t pluck_cutoff_hz;    // excitation low-pass at full velocity
  real_t velocity_tone;      // 0..1: how much softer notes darken the pluck
  real_t noise;              // 0..1 noise share in the pluck
  BodyMode body[6];
  int body_modes;
  // Unison: a second string, detuned (harpsichord choirs, piano unisons). 1 or 2.
  int strings;
  real_t detune_cents;
  // Key release
  real_t release_t60_s;      // decay after key-up (dampers)
  real_t release_noise;      // level of the damper / quill-return noise burst
  real_t gain;
};

class WaveguideStringData : public InstrumentData {};

// Digital-waveguide plucked/struck string (single delay loop, Jaffe-Smith extensions):
//  - exact tuning: the phase delay of every loop filter is computed at the fundamental and
//    the remainder goes to a first-order allpass fractional delay solved for that frequency
//  - per-note decay times from T60 targets, frequency-dependent loss from the loop filter
//  - optional dispersion (stiffness) and a detuned second string
//  - commuted body: the pluck is filtered by body resonances before it enters the string
class WaveguideString : public InstrumentProcessor {
 public:
  explicit WaveguideString(const WaveguideParams& params);
  ~WaveguideString();

  int Load() override;
  int Unload() override;
  InstrumentData* NewInstrumentData() override { return new WaveguideStringData(); }
  real_t Tick(int v) override;
  int SetFrequency(real_t freq, int v) override;
  int NoteOn(int v) override;
  int NoteOff(int v) override;
  void set_instrument_data(InstrumentData* idata) override { cdata_ = idata; }
  bool IsVoiceActive(int v) const override { return voices_[v].active; }
  real_t VoiceLevel(int v) const override { return voices_[v].level; }
  void BendFrequency(real_t freq, int v) override;

  // Presets
  static WaveguideParams Harpsichord();
  static WaveguideParams Piano();
  static WaveguideParams NylonGuitar();
  static WaveguideParams SteelGuitar();
  static WaveguideParams MutedGuitar();
  static WaveguideParams Bass();
  static WaveguideParams Harp();

 private:
  static const int kMaxStrings = 2;
  static const int kMaxStiffness = 4;
  static const int kExcitationLength = 4096;

  struct String {
    float* line;          // delay line (ring buffer)
    int write;
    int delay;            // integer part of the loop delay
    float frac_coef;      // fractional-delay allpass coefficient
    float frac_x1, frac_y1;
    float stiff_x1[kMaxStiffness], stiff_y1[kMaxStiffness];
    float damping;        // loop filter S
    float lp_x1;
    float gain_held, gain_released, gain;
  };
  struct Voice {
    String strings[kMaxStrings];
    float excitation[kExcitationLength];
    int exc_pos, exc_len;
    float dc_x1, dc_y1;   // output DC blocker
    float level;          // peak follower for activity / stealing
    real_t freq;
    bool active;
    bool released;
  };

  void Tune(Voice& voice, real_t freq, bool reset_state);
  void BuildExcitation(Voice& voice, real_t freq, real_t velocity, real_t amplitude, bool with_body);
  float Random();

  WaveguideParams params_;
  Voice* voices_;          // Polyphony voices
  int line_capacity_;
  float stiffness_coef_;
  float dc_coef_, level_decay_;
  uint32_t seed_;
  InstrumentData* cdata_;
};

}
}
}
}
