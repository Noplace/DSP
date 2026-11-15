#pragma once

#include <math.h>

namespace dsp {
namespace audio {
namespace synth {
namespace effects {

// Second-order IIR sections designed with the RBJ "Audio EQ Cookbook" formulas.
// Coefficients are normalised so a0 = 1.
struct BiquadCoefs {
  double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0;
};

enum class BiquadType { kLowPass, kHighPass, kPeak, kLowShelf, kHighShelf };

inline BiquadCoefs DesignBiquad(BiquadType type, double sample_rate, double freq, double q, double gain_db) {
  const double kPi = 3.14159265358979323846;
  if (freq > sample_rate * 0.45) freq = sample_rate * 0.45;
  if (freq < 5.0) freq = 5.0;
  if (q < 0.05) q = 0.05;
  const double w0 = 2.0 * kPi * freq / sample_rate;
  const double cw = cos(w0), sw = sin(w0);
  const double alpha = sw / (2.0 * q);
  const double A = pow(10.0, gain_db / 40.0);
  double b0 = 1, b1 = 0, b2 = 0, a0 = 1, a1 = 0, a2 = 0;
  switch (type) {
    case BiquadType::kLowPass:
      b0 = (1 - cw) * 0.5; b1 = 1 - cw; b2 = (1 - cw) * 0.5;
      a0 = 1 + alpha; a1 = -2 * cw; a2 = 1 - alpha;
      break;
    case BiquadType::kHighPass:
      b0 = (1 + cw) * 0.5; b1 = -(1 + cw); b2 = (1 + cw) * 0.5;
      a0 = 1 + alpha; a1 = -2 * cw; a2 = 1 - alpha;
      break;
    case BiquadType::kPeak:
      b0 = 1 + alpha * A; b1 = -2 * cw; b2 = 1 - alpha * A;
      a0 = 1 + alpha / A; a1 = -2 * cw; a2 = 1 - alpha / A;
      break;
    case BiquadType::kLowShelf: {
      const double sa = 2 * sqrt(A) * alpha;
      b0 = A * ((A + 1) - (A - 1) * cw + sa);
      b1 = 2 * A * ((A - 1) - (A + 1) * cw);
      b2 = A * ((A + 1) - (A - 1) * cw - sa);
      a0 = (A + 1) + (A - 1) * cw + sa;
      a1 = -2 * ((A - 1) + (A + 1) * cw);
      a2 = (A + 1) + (A - 1) * cw - sa;
      break;
    }
    case BiquadType::kHighShelf: {
      const double sa = 2 * sqrt(A) * alpha;
      b0 = A * ((A + 1) + (A - 1) * cw + sa);
      b1 = -2 * A * ((A - 1) + (A + 1) * cw);
      b2 = A * ((A + 1) + (A - 1) * cw - sa);
      a0 = (A + 1) - (A - 1) * cw + sa;
      a1 = 2 * ((A - 1) - (A + 1) * cw);
      a2 = (A + 1) - (A - 1) * cw - sa;
      break;
    }
  }
  BiquadCoefs c;
  c.b0 = b0 / a0; c.b1 = b1 / a0; c.b2 = b2 / a0;
  c.a1 = a1 / a0; c.a2 = a2 / a0;
  return c;
}

// |H(e^jw)| in dB at freq.
inline double BiquadMagnitudeDb(const BiquadCoefs& c, double sample_rate, double freq) {
  const double kPi = 3.14159265358979323846;
  const double w = 2.0 * kPi * freq / sample_rate;
  const double c1 = cos(w), s1 = sin(w), c2 = cos(2 * w), s2 = sin(2 * w);
  const double nr = c.b0 + c.b1 * c1 + c.b2 * c2, ni = -(c.b1 * s1 + c.b2 * s2);
  const double dr = 1.0 + c.a1 * c1 + c.a2 * c2, di = -(c.a1 * s1 + c.a2 * s2);
  const double num = nr * nr + ni * ni, den = dr * dr + di * di;
  if (den <= 0 || num <= 0) return -120.0;
  return 10.0 * log10(num / den);
}

// Stereo biquad, transposed direct form II with double state.
struct StereoBiquad {
  BiquadCoefs c;
  double z1[2] = {0, 0}, z2[2] = {0, 0};

  void Reset() { z1[0] = z1[1] = z2[0] = z2[1] = 0; }
  inline float Tick(int ch, float x) {
    const double y = c.b0 * x + z1[ch];
    z1[ch] = c.b1 * x - c.a1 * y + z2[ch];
    z2[ch] = c.b2 * x - c.a2 * y;
    return float(y);
  }
  inline void Process(float& l, float& r) {
    l = Tick(0, l);
    r = Tick(1, r);
  }
};

}
}
}
}
