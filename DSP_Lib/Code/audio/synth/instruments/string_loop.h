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

namespace dsp {
namespace audio {
namespace synth {
namespace instruments {

// Single-delay-loop string, designed from physical measurements (shared by the synthesizer and
// by the calibration tool that inverse-filters recordings with exactly the same loop).
//
//   y = e + L(y),  L = loss( dispersion( fractional_delay( z^-delay ) ) )
//
// loss:        g (1 + a) / (1 + a z^-1). Per-period gain exp(-sigma(f)/f0) with the measured decay
//              rate sigma(f) = b1 + b3 f^2 (1/s) -- the shape a one-pole gives at low frequencies.
// dispersion:  so the partials follow f_n = n f0 sqrt(1 + B n^2) (string stiffness), either
//              `stages` identical first-order allpasses (c + z^-1)/(1 + c z^-1) -- enough for small B --
//              or `biquads` second-order allpass sections placed by the Abel-Smith method (one
//              section per 2 pi of required phase; piano strings).
// tuning:      the phase delay of everything at f0 is subtracted from the period and the remainder
//              goes to the integer delay plus a first-order allpass fractional delay.
const int kMaxDispersionStages = 16;
const int kMaxDispersionBiquads = 48;

struct LoopDesign {
  int delay;
  float frac;          // fractional-delay allpass coefficient
  float disp;          // dispersion allpass coefficient
  int stages;          // number of first-order dispersion allpasses (0 = none)
  int biquads;         // number of second-order dispersion sections (0 = none)
  float bq_a1[kMaxDispersionBiquads], bq_a2[kMaxDispersionBiquads];  // (a2 + a1 z^-1 + z^-2)/(1 + a1 z^-1 + a2 z^-2)
  float loss_g, loss_a;
  double max_partial_error_cents;  // fit quality of the dispersion (first partials up to 10 kHz)
  double rms_partial_error_cents;  // same, RMS weighted by 1/n
};

struct LoopParams {
  double f0_hz;
  double inharmonicity;   // B
  double b1, b3;          // decay rate model, 1/s and 1/(s Hz^2)
};

LoopDesign DesignLoop(double sample_rate, const LoopParams& params, int max_stages);

// Most dispersion stages a note may use (the design uses the fewest that fit nearly as well).
inline int DispersionStagesFor(double) { return kMaxDispersionStages; }  // the design picks the fewest that fit

// Re-tunes an existing design to a new fundamental (pitch bend), keeping loss and dispersion.
// Cheap enough for the audio thread.
LoopDesign RetuneLoop(const LoopDesign& design, double sample_rate, double f0_hz);

// A design with the dispersion of `like` but its own fundamental and decay (a second, slightly
// detuned string of the same note): no dispersion search, so it is quick.
LoopDesign DesignLoopLike(const LoopDesign& like, double sample_rate, const LoopParams& params);

// Resonances of a loop (its partials 1..count below Nyquist): frequency (radians per sample), pole
// radius per sample and group delay (samples). A partial of the loop then behaves exactly like a
// one-pole resonator there with weight 2 / group delay. Returns how many were found.
struct LoopResonance {
  double w, radius, group_delay;
};
int FindLoopResonances(const LoopDesign& design, int count, LoopResonance* out);

// Per-period gain multiplier that turns the loop's held decay into a T60 of `t60_s` (dampers).
float ReleaseGain(double f0_hz, double t60_s);

class StringLoop {
 public:
  StringLoop() : line_(nullptr), capacity_(0), write_(0), design_() { Reset(); }
  StringLoop(const StringLoop&) = delete;
  StringLoop& operator=(const StringLoop&) = delete;
  ~StringLoop() { delete[] line_; }
  void Allocate(int capacity);   // max delay in samples (+ margin)
  void SetDesign(const LoopDesign& d) { design_ = d; }
  const LoopDesign& design() const { return design_; }
  void SetLossGain(float g) { design_.loss_g = g; }
  void Reset();                  // silence (clears the active part of the delay line and filter states)

  // Synthesis: returns the string output for excitation sample `in`.
  inline float Tick(float in) {
    float v = Loop();
    float out = v + in;
    line_[write_] = out;
    if (++write_ == capacity_) write_ = 0;
    return out;
  }
  // Inverse filter: given the recorded output y, returns the excitation e = y - L(y).
  inline float Inverse(float y) {
    float v = Loop();
    line_[write_] = y;
    if (++write_ == capacity_) write_ = 0;
    return y - v;
  }

 private:
  inline float Loop() {
    int r = write_ - design_.delay;
    if (r < 0) r += capacity_;
    float x = line_[r];
    float y = design_.frac * x + frac_x1_ - design_.frac * frac_y1_;
    frac_x1_ = x; frac_y1_ = y;
    const float c = design_.disp;
    for (int k = 0; k < design_.stages; ++k) {
      float z = c * y + disp_x1_[k] - c * disp_y1_[k];
      disp_x1_[k] = y; disp_y1_[k] = z;
      y = z;
    }
    for (int k = 0; k < design_.biquads; ++k) {  // transposed direct form II allpass
      const float a1 = design_.bq_a1[k], a2 = design_.bq_a2[k];
      float z = a2 * y + bq_s1_[k];
      bq_s1_[k] = a1 * (y - z) + bq_s2_[k];
      bq_s2_[k] = y - a2 * z;
      y = z;
    }
    float l = design_.loss_g * (1.0f + design_.loss_a) * y - design_.loss_a * loss_y1_;
    loss_y1_ = l;
    return l;
  }

  float* line_;
  int capacity_, write_;
  LoopDesign design_;
  float frac_x1_, frac_y1_, loss_y1_;
  float disp_x1_[kMaxDispersionStages], disp_y1_[kMaxDispersionStages];
  float bq_s1_[kMaxDispersionBiquads], bq_s2_[kMaxDispersionBiquads];
};

}
}
}
}
