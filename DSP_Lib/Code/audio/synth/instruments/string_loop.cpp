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
#include <algorithm>
#include <complex>
#include <math.h>
#include <string.h>
#include <vector>
#include "string_loop.h"

namespace dsp {
namespace audio {
namespace synth {
namespace instruments {

namespace {

const double kPi = 3.14159265358979323846;

// Phase delay (samples) at w of the first-order allpass (c + z^-1)/(1 + c z^-1).
double AllpassDelay(double c, double w) {
  std::complex<double> z1 = std::polar(1.0, -w);
  return -std::arg((c + z1) / (1.0 + c * z1)) / w;
}
// Phase delay at w of the one-pole loss filter g(1+a)/(1 + a z^-1).
double LossDelay(double a, double w) {
  return std::arg(1.0 + a * std::polar(1.0, -w)) / w;
}
double SolveAllpass(double delay, double w) {
  double lo = -0.9999, hi = 0.9999;
  for (int i = 0; i < 48; ++i) {
    double mid = 0.5 * (lo + hi);
    if (AllpassDelay(mid, w) > delay) lo = mid; else hi = mid;
  }
  return 0.5 * (lo + hi);
}

struct Candidate {
  LoopDesign design;
  bool feasible;
};

// Completes a design for a given dispersion coefficient: tune the fundamental exactly.
Candidate Complete(double sr, double f0, double loss_a, double c, int stages) {
  Candidate out = {};
  const double w0 = 2 * kPi * f0 / sr;
  const double period = sr / f0;
  double remaining = period - LossDelay(loss_a, w0) - (stages ? stages * AllpassDelay(c, w0) : 0.0);
  int delay = int(floor(remaining - 0.5));
  out.feasible = delay >= 1;
  if (!out.feasible) return out;
  out.design.delay = delay;
  out.design.frac = float(SolveAllpass(remaining - delay, w0));
  out.design.disp = float(c);
  out.design.stages = stages;
  out.design.loss_a = float(loss_a);
  return out;
}

// Total loop phase delay at w for a design.
double LoopDelay(const LoopDesign& d, double w) {
  return d.delay + AllpassDelay(d.frac, w) + LossDelay(d.loss_a, w) + (d.stages ? d.stages * AllpassDelay(d.disp, w) : 0.0);
}

// Largest |cents| error of the model's partials against the stiff-string series (relative to the
// fundamental), over the partials below min(10 kHz, 0.45 sr).
double PartialError(const LoopDesign& d, double sr, double f0, double B) {
  double worst = 0;
  const int max_n = std::min(40, int(std::min(10000.0, 0.45 * sr) / f0));
  for (int n = 2; n <= max_n; ++n) {
    double target = n * f0 * sqrt((1 + B * n * n) / (1 + B));
    double f = target;
    for (int it = 0; it < 6; ++it)  // resonance: f = n sr / D(f)
      f = n * sr / LoopDelay(d, 2 * kPi * f / sr);
    worst = std::max(worst, fabs(1200 * log2(f / target)));
  }
  return worst;
}

// Fits the one-pole loss g(1+a)/(1 + a z^-1) to the target decay at the partial frequencies.
// Per period, a partial at w loses  -ln g + c(w, a)  nepers, with c = -ln|(1+a)/(1 + a e^-jw)|;
// the target is sigma(f)/f0. For a given a the best -ln g has a closed form (least squares on the
// relative error); a is found by golden-section search.
void FitLoss(double sr, double f0, double b1, double b3, double& g_out, double& a_out) {
  std::vector<double> w, target;
  const int max_n = std::min(40, int(0.45 * sr / f0));
  for (int n = 1; n <= std::max(1, max_n); ++n) {
    double f = n * f0;
    w.push_back(2 * kPi * f / sr);
    target.push_back(std::max(1e-6, (b1 + b3 * f * f) / f0));
  }
  auto evaluate = [&](double a, double* x_out) {
    double num = 0, den = 0;
    std::vector<double> c(w.size());
    for (size_t i = 0; i < w.size(); ++i) {
      c[i] = -log(std::abs((1.0 + a) / (1.0 + a * std::polar(1.0, -w[i]))));
      num += (target[i] - c[i]) / (target[i] * target[i]);
      den += 1.0 / (target[i] * target[i]);
    }
    double x = std::max(0.0, num / den);
    double err = 0;
    for (size_t i = 0; i < w.size(); ++i) {
      double e = (x + c[i] - target[i]) / target[i];
      err += e * e;
    }
    if (x_out) *x_out = x;
    return err;
  };
  double lo = -0.95, hi = 0.0;
  const double phi = 0.6180339887;
  double x1 = hi - phi * (hi - lo), x2 = lo + phi * (hi - lo);
  double e1 = evaluate(x1, nullptr), e2 = evaluate(x2, nullptr);
  for (int it = 0; it < 40; ++it) {
    if (e1 < e2) { hi = x2; x2 = x1; e2 = e1; x1 = hi - phi * (hi - lo); e1 = evaluate(x1, nullptr); }
    else { lo = x1; x1 = x2; e1 = e2; x2 = lo + phi * (hi - lo); e2 = evaluate(x2, nullptr); }
  }
  a_out = e1 < e2 ? x1 : x2;
  double x;
  evaluate(a_out, &x);
  g_out = exp(-x);
}

}  // namespace

LoopDesign DesignLoop(double sr, const LoopParams& p, int max_stages) {
  const double f0 = p.f0_hz;
  double g, loss_a;
  FitLoss(sr, f0, std::max(0.0, p.b1), std::max(0.0, p.b3), g, loss_a);

  int stages = (p.inharmonicity > 1e-7) ? std::min(max_stages, kMaxDispersionStages) : 0;
  Candidate best = Complete(sr, f0, loss_a, 0.0, 0);
  double best_err = PartialError(best.design, sr, f0, p.inharmonicity);
  if (stages > 0) {
    // Golden-section search on the dispersion coefficient (more negative = more stretching).
    double lo = -0.95, hi = 0.0;
    const double phi = 0.6180339887;
    auto err = [&](double c) {
      Candidate cand = Complete(sr, f0, loss_a, c, stages);
      return cand.feasible ? PartialError(cand.design, sr, f0, p.inharmonicity) : 1e9;
    };
    double x1 = hi - phi * (hi - lo), x2 = lo + phi * (hi - lo);
    double e1 = err(x1), e2 = err(x2);
    for (int it = 0; it < 30; ++it) {
      if (e1 < e2) { hi = x2; x2 = x1; e2 = e1; x1 = hi - phi * (hi - lo); e1 = err(x1); }
      else { lo = x1; x1 = x2; e1 = e2; x2 = lo + phi * (hi - lo); e2 = err(x2); }
    }
    double c = e1 < e2 ? x1 : x2;
    Candidate cand = Complete(sr, f0, loss_a, c, stages);
    double e = cand.feasible ? PartialError(cand.design, sr, f0, p.inharmonicity) : 1e9;
    if (e < best_err) { best = cand; best_err = e; }
  }
  best.design.loss_g = float(std::min(g, 0.99999));
  best.design.max_partial_error_cents = best_err;
  return best.design;
}

LoopDesign RetuneLoop(const LoopDesign& d, double sr, double f0) {
  Candidate c = Complete(sr, f0, d.loss_a, d.disp, d.stages);
  if (!c.feasible) return d;
  c.design.loss_g = d.loss_g;
  c.design.max_partial_error_cents = d.max_partial_error_cents;
  return c.design;
}

float ReleaseGain(double f0_hz, double t60_s) {
  return float(pow(10.0, -3.0 / (f0_hz * std::max(0.005, t60_s))));
}

void StringLoop::Allocate(int capacity) {
  delete[] line_;
  capacity_ = capacity;
  line_ = new float[capacity_];
  memset(line_, 0, sizeof(float) * capacity_);
  write_ = 0;
}

void StringLoop::Reset() {
  if (line_) {
    memset(line_, 0, sizeof(float) * capacity_);
    write_ = 0;
  }
  frac_x1_ = frac_y1_ = loss_y1_ = 0;
  memset(disp_x1_, 0, sizeof(disp_x1_));
  memset(disp_y1_, 0, sizeof(disp_y1_));
}

}
}
}
}
