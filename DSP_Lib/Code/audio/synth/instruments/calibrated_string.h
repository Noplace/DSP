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

// A plucked or struck string calibrated from recordings of a real instrument (commuted waveguide
// synthesis):
//  - string loop per note from measured inharmonicity and decay (smoothed across the keyboard)
//  - excitation = the real pluck/hammer + soundboard response, recovered by inverse-filtering the
//    recording with that same loop, pitch-shifted to each note from the nearest recorded one
//  - velocity layers (piano): the excitations of the two nearest layers are crossfaded, so tone and
//    loudness follow the recordings (the channel's generic velocity curve is bypassed)
//  - low partials (piano) as resonators with each partial's measured decay: a fast first stage and
//    a long, quiet aftersound, as the unison strings and the soundboard trade energy. A crossover
//    splits the excitation between them and the string loop, which keeps the upper partials; the
//    two halves add up to the plain string at the onset.
//  - optional second string: an identical one out of tune (honky-tonk), or, for calibrations
//    without per-partial decays, a slightly detuned one with the measured aftersound decay
struct CalibratedStringOptions {
  real_t release_t60_s = 0.15f;   // decay after key-up (dampers)
  real_t level = 0.35f;           // output level for a typical note (loudest layer)
  real_t detune_cents = 0.0f;     // second string detune when the calibration has an aftersound
  real_t min_velocity_gain_db = -18.0f;  // velocities below the softest layer fade down to this
  bool recorded_tuning = false;   // follow the instrument's own (e.g. stretched piano) tuning
  int undamped_from_note = 128;   // notes from here up have no dampers (piano treble)
  real_t modal_max_hz = 1500.0f;  // partials up to here play as resonators (0 = string loop only)
  int modal_max_partials = 20;
  real_t unison_detune_cents = 0.0f;  // != 0: a second, identical string this far out of tune
};

class CalibratedString : public InstrumentProcessor {
 public:
  CalibratedString(std::shared_ptr<const StringCalibration> calibration, const CalibratedStringOptions& options);
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
  bool HandlesVelocity() const override { return layered_; }

  // Smoothed parameters for a MIDI note (for tools/inspection).
  LoopParams NoteParams(int midi_note) const;

 private:
  struct Layer {
    float velocity;
    std::vector<float> samples;   // at the synth's sample rate, transposed to the note
  };
  static const int kMaxModalPartials = 24;
  static const int kMaxModes = 3 * kMaxModalPartials;  // first stage (x2 with a unison) + aftersound
  // Decay of each resonator partial for one velocity layer (rates in 1/s).
  struct ModalLayer {
    float sigma[kMaxModalPartials];
    float after_sigma[kMaxModalPartials];
    float after_level[kMaxModalPartials];   // 0 = single stage
  };
  struct NoteModal {
    int partials = 0;                       // 0 = no resonators for this note
    float w[kMaxModalPartials];             // the loop's partial frequencies (radians per sample, unbent)
    float r_loop[kMaxModalPartials];        // ... and pole radii: the resonators start out as the loop's partials
    float coef[kMaxModalPartials];          // 2 / loop group delay: a resonator then matches the string partial
    float hp[2][5];                         // crossover: 4th-order high-pass ahead of the loop (b0 b1 b2 a1 a2)
    std::vector<ModalLayer> layers;         // same order as the excitation layers
  };
  struct Voice {
    StringLoop loop, loop_b;      // loop_b: second string (aftersound), if used
    const float* exc_lo;
    const float* exc_hi;
    int len_lo, len_hi, exc_pos;
    float w_lo, w_hi;             // crossfade between the two layers (gain included)
    float share_b;                // excitation share fed to the second string
    bool two_strings;
    float dc_x1, dc_y1;
    float level;
    float base_gain, base_gain_b;
    int note;
    real_t freq;
    bool active, released;
    // Resonators: complex one-pole each (re, im), pole p = r e^(jw), output weight c.
    int modes;
    float m_re[kMaxModes], m_im[kMaxModes], m_pr[kMaxModes], m_pi[kMaxModes];
    float m_c[kMaxModes], m_r[kMaxModes], m_w[kMaxModes];
    float m_rt[kMaxModes];                  // radius once the excitation has played (the measured decay)
    bool modes_settled;
    double mode_f0;                         // fundamental the resonator angles were set for
    const float (*hp)[5];
    float hp_s[2][2];
    int hp_run;                             // crossover samples left after the excitation ends
  };

  void BuildTables();
  void BuildModal(int m, const CalibratedNote& src);
  void StartModes(Voice& voice, int note, size_t lo, size_t hi, float t, double f0);

  std::shared_ptr<const StringCalibration> cal_;
  CalibratedStringOptions options_;
  float gain_;                                 // normalises the recordings' level
  bool layered_;
  LoopDesign designs_[128], designs_b_[128];
  LoopParams params_[128];
  float after_level_[128];
  NoteModal modal_[128];
  real_t b_detune_cents_;                      // second string vs the first
  float tuning_ratio_[128];                    // recorded tuning vs equal temperament (or 1)
  std::vector<Layer> layers_[128];             // per MIDI note, sorted by velocity
  Voice* voices_;
  float dc_coef_, level_decay_;
  InstrumentData* cdata_;
};

}
}
}
}
