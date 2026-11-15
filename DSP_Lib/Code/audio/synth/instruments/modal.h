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

// One vibration mode. In a pitched instrument `freq` is a ratio to the note frequency and
// `t60` scales the note's decay; in a drum it is in Hz and seconds.
struct Mode {
  real_t freq;
  real_t gain;
  real_t t60;
};

// Everything that makes one sound: modes + noise + click + pitch drop.
struct ModalSound {
  static const int kMaxModes = 16;
  Mode modes[kMaxModes];
  int mode_count;
  real_t mallet_cutoff_hz;   // mallet/stick hardness at full velocity: modes above it are softer
  real_t attack_ms;          // soft mallets have a short rise
  real_t sweep;              // pitch drop: starts at (1 + sweep) x frequency ...
  real_t sweep_ms;           // ... and settles with this time constant (kick, toms)
  real_t noise;              // filtered-noise layer (snare wires, cymbal wash, mallet thud)
  real_t noise_hz, noise_q, noise_t60;
  real_t click;              // very short broadband click (beater / stick contact)
  int choke_group;           // a new note in the same group damps the others (hi-hats)
  real_t gain;
};

struct ModalParams {
  ModalSound sound;          // pitched: mode freq = ratio, t60 = scale of the note's T60
  real_t t60_c4_s;           // fundamental decay at middle C ...
  real_t t60_slope;          // ... scaled by (261.6 / f)^slope
  real_t release_t60_s;      // decay after key-up (dampers); 0 = keep ringing
  real_t tremolo_hz, tremolo_depth;  // vibraphone motor
};

class ModalData : public InstrumentData {};

// Modal synthesis: a struck object is a sum of exponentially decaying sinusoids (its modes).
// Each mode is a rotating complex phasor, ~6 multiply-adds per sample.
//  - pitched mode (mallets, bells, timpani): modes are ratios of the note frequency
//  - kit mode (GM drum channel): each note number selects its own ModalSound
class ModalInstrument : public InstrumentProcessor {
 public:
  explicit ModalInstrument(const ModalParams& params);  // pitched
  static ModalInstrument* NewDrumKit();                  // GM percussion map (notes 27-87)
  ~ModalInstrument();

  int Load() override;
  int Unload() override;
  InstrumentData* NewInstrumentData() override { return new ModalData(); }
  real_t Tick(int v) override;
  int SetFrequency(real_t freq, int v) override;
  int NoteOn(int v) override;
  int NoteOff(int v) override;
  void set_instrument_data(InstrumentData* idata) override { cdata_ = idata; }
  bool IsVoiceActive(int v) const override { return voices_ && voices_[v].active; }
  real_t VoiceLevel(int v) const override { return voices_ ? voices_[v].level : 0; }
  void BendFrequency(real_t freq, int v) override;

  // Pitched presets
  static ModalParams Marimba();
  static ModalParams Xylophone();
  static ModalParams Vibraphone();
  static ModalParams Glockenspiel();
  static ModalParams Celesta();
  static ModalParams MusicBox();
  static ModalParams TubularBells();
  static ModalParams SteelDrums();
  static ModalParams Kalimba();
  static ModalParams Timpani();
  static ModalParams Agogo();
  static ModalParams Woodblock();
  static ModalParams Taiko();
  static ModalParams SynthDrum();

  // Name of a GM drum note (for tools), or nullptr if unmapped.
  static const char* DrumName(int note);

 private:
  struct VoiceMode {
    float re, im;   // phasor; output is im
    float c, s;     // rotation (includes the per-sample decay r)
    float w;        // base radian frequency
    float r;        // per-sample decay
  };
  struct Voice {
    VoiceMode modes[ModalSound::kMaxModes];
    int mode_count;
    float sweep_amount, sweep_env, sweep_coef16;
    int block_counter;
    float noise_env, noise_coef;
    float b0, b2, a1, a2, z1, z2;   // band-pass biquad (b1 = 0)
    float click_env, click_coef;
    float attack, attack_coef;
    float trem_phase, trem_inc, trem_depth;
    float level, gain;
    float freq;
    int choke_group;
    bool active;
  };

  ModalInstrument();
  void Start(Voice& voice, const ModalSound& sound, real_t f0, real_t velocity, bool pitched);
  void Damp(Voice& voice, real_t t60);
  void UpdateRotation(Voice& voice, float multiplier);
  float Random();

  bool kit_;
  ModalParams params_;
  const ModalSound* kit_sounds_;   // indexed by note - kKitFirstNote
  Voice* voices_;
  uint32_t seed_;
  float level_decay_;
  InstrumentData* cdata_;
};

}
}
}
}
