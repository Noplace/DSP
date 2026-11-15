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
#include <stdio.h>
#include <stdlib.h>
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

// Phase delay at w of the second-order allpass (a2 + a1 z^-1 + z^-2)/(1 + a1 z^-1 + a2 z^-2).
// Its phase is -2w - 2 arg(D(e^jw)) with D = 1 + a1 e^-jw + a2 e^-2jw (continuous for stable D).
double BiquadDelay(double a1, double a2, double w) {
  std::complex<double> z1 = std::polar(1.0, -w);
  return (2 * w + 2 * std::arg(1.0 + a1 * z1 + a2 * z1 * z1)) / w;
}

double DispersionDelay(const LoopDesign& d, double w) {
  double t = d.stages ? d.stages * AllpassDelay(d.disp, w) : 0.0;
  for (int k = 0; k < d.biquads; ++k) t += BiquadDelay(d.bq_a1[k], d.bq_a2[k], w);
  return t;
}

struct Candidate {
  LoopDesign design;
  bool feasible;
};

// Completes a design whose loss and dispersion are set: tune the fundamental exactly with the
// integer delay plus the fractional-delay allpass.
Candidate Complete(double sr, double f0, const LoopDesign& base) {
  Candidate out = {};
  out.design = base;
  const double w0 = 2 * kPi * f0 / sr;
  const double period = sr / f0;
  double remaining = period - LossDelay(base.loss_a, w0) - DispersionDelay(base, w0);
  int delay = int(floor(remaining - 0.5));
  out.feasible = delay >= 1;
  if (!out.feasible) return out;
  out.design.delay = delay;
  out.design.frac = float(SolveAllpass(remaining - delay, w0));
  return out;
}

// Variant for a cascade of identical first-order allpasses.
Candidate Complete(double sr, double f0, double loss_a, double c, int stages) {
  LoopDesign base = {};
  base.loss_a = float(loss_a);
  base.disp = float(c);
  base.stages = stages;
  return Complete(sr, f0, base);
}

// Total loop phase delay at w for a design.
double LoopDelay(const LoopDesign& d, double w) {
  return d.delay + AllpassDelay(d.frac, w) + LossDelay(d.loss_a, w) + DispersionDelay(d, w);
}

// Partial number (continuous) at frequency f for the stiff-string series with the fundamental at
// f0: f_n = n f0 sqrt((1 + B n^2) / (1 + B)).
double PartialNumber(double f, double f0, double B) {
  double u = f / f0 * sqrt(1 + B);
  if (B < 1e-12) return u;
  return sqrt((-1 + sqrt(1 + 4 * B * u * u)) / (2 * B));
}

// Abel & Smith, "Robust design of very high-order allpass dispersion filters" (DAFx 2006):
// the loop must lag 2 pi n at partial n. Subtract a linear phase (the delay line) so the remaining
// lag rises monotonically to 2 pi N at Nyquist, then put one second-order allpass section where it
// crosses each 2 pi (k - 1/2), with a pole radius from the spacing to its neighbours (alpha scales
// the bandwidth). Above w_cap the target is continued linearly (fewer sections, high partials
// matter less). The pole frequencies are then refined iteratively against the actual cascade
// phase. Returns false if no sections are needed or more than kMaxDispersionBiquads would be.
bool AbelSmith(double sr, double f0, double B, double loss_a, double w_cap, double alpha, LoopDesign& d,
               int extra_sections = 0) {
  const int K = 2048;
  std::vector<double> w(K + 1), g(K + 1);
  auto target = [&](double wi) {
    return 2 * kPi * PartialNumber(wi * sr / (2 * kPi), f0, B);
  };
  const double h = 1e-4;
  const double phi_cap = target(w_cap), slope_cap = (target(w_cap) - target(w_cap - h)) / h;
  for (int i = 0; i <= K; ++i) {
    w[i] = kPi * i / K;
    double phi = w[i] <= w_cap ? target(w[i]) : phi_cap + slope_cap * (w[i] - w_cap);
    g[i] = phi - (i ? w[i] * LossDelay(loss_a, w[i]) : 0.0);
  }
  const double slope_end = (g[K] - g[K - 1]) / (w[K] - w[K - 1]);
  // extra_sections move 2 samples each from the delay line into the allpasses: more, narrower
  // sections follow the stretch curve more closely.
  int N = int(floor((g[K] - kPi * slope_end) / (2 * kPi)));
  if (N > 0) N += extra_sections;
  if (N <= 0) return false;
  if (N > kMaxDispersionBiquads) return false;
  const double D = (g[K] - 2 * kPi * N) / kPi;
  std::vector<double> poles;
  int i = 1;
  for (int k = 1; k <= N; ++k) {
    double level = 2 * kPi * (k - 0.5);
    while (i < K && g[i] - w[i] * D < level) ++i;
    poles.push_back(w[i]);
  }
  // Target dispersion lag at any w (linear interpolation on the grid).
  auto target_lag = [&](double wi) {
    double pos = wi / kPi * K;
    int j = std::min(K - 1, std::max(0, int(pos)));
    double t = pos - j;
    double a = g[j] - w[j] * D, b = g[j + 1] - w[j + 1] * D;
    return a + t * (b - a);
  };
  std::vector<double> rho(N);
  auto set_radii = [&]() {
    for (int k = 0; k < N; ++k) {
      double lo = k > 0 ? poles[k - 1] : 0.0;
      double hi = k + 1 < N ? poles[k + 1] : kPi;
      double spacing = 0.5 * (hi - lo);
      rho[k] = std::min(0.9995, exp(-alpha * spacing / 2));
    }
  };
  auto section_lag = [&](int k, double wi) {
    double a1 = -2 * rho[k] * cos(poles[k]), a2 = rho[k] * rho[k];
    std::complex<double> z1 = std::polar(1.0, -wi);
    return 2 * wi + 2 * std::arg(1.0 + a1 * z1 + a2 * z1 * z1);
  };
  set_radii();
  // Refinement: with broad sections a section's lag at its own pole is not exactly pi (its
  // neighbours' tails overlap), so compare the cascade's actual lag with the target at each pole
  // and move the pole to cancel the difference; repeat.
  for (int iter = 0; iter < 20; ++iter) {
    std::vector<double> moved(poles);
    for (int k = 0; k < N; ++k) {
      double actual = 0;
      for (int j = 0; j < N; ++j) actual += section_lag(j, poles[k]);
      double err = actual - target_lag(poles[k]);
      double h2 = 1e-3;
      double slope = (target_lag(std::min(kPi, poles[k] + h2)) - target_lag(std::max(0.0, poles[k] - h2))) / (2 * h2);
      if (slope > 1e-6) moved[k] = poles[k] + 0.7 * err / slope;
    }
    for (int k = 0; k < N; ++k) {  // keep the poles ordered and inside (0, pi)
      double lo = k > 0 ? moved[k - 1] : 0.0;
      moved[k] = std::min(kPi * 0.999, std::max(lo + 1e-4, moved[k]));
    }
    poles = moved;
    set_radii();
  }
  d.biquads = N;
  for (int k = 0; k < N; ++k) {
    d.bq_a1[k] = float(-2 * rho[k] * cos(poles[k]));
    d.bq_a2[k] = float(rho[k] * rho[k]);
  }
  return true;
}

// Error (cents) of the model's partials against the stiff-string series (relative to the
// fundamental), over the partials below min(10 kHz, 0.45 sr): the largest one, or (rms = true)
// an RMS weighted by 1/n -- the low partials carry most of the energy and of the pitch percept.
double PartialError(const LoopDesign& d, double sr, double f0, double B, bool rms = false) {
  double worst = 0, sum = 0, wsum = 0;
  const int max_n = std::min(40, int(std::min(10000.0, 0.45 * sr) / f0));
  for (int n = 2; n <= max_n; ++n) {
    double target = n * f0 * sqrt((1 + B * n * n) / (1 + B));
    double f = target;
    for (int it = 0; it < 6; ++it)  // resonance: f = n sr / D(f)
      f = n * sr / LoopDelay(d, 2 * kPi * f / sr);
    double e = 1200 * log2(f / target);
    worst = std::max(worst, fabs(e));
    sum += e * e / n;
    wsum += 1.0 / n;
  }
  return rms ? (wsum > 0 ? sqrt(sum / wsum) : 0.0) : worst;
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

  const double B = p.inharmonicity;
  const int limit = std::min(max_stages, kMaxDispersionStages);
  // Candidates: no dispersion; cascades of 2..16 identical first-order allpasses, each with its best
  // coefficient; Abel-Smith second-order designs. Keep the cheapest that fits almost as well as the
  // best (cost ~ multiplies per sample).
  struct Option { Candidate cand; double rms; int cost; };
  std::vector<Option> options;
  {
    Candidate none = Complete(sr, f0, loss_a, 0.0, 0);
    options.push_back({none, PartialError(none.design, sr, f0, B, true), 0});
  }
  if (B > 1e-7) {
    // Follow the stretch exactly up to w_cap; deep bass strings need too many sections for 10 kHz,
    // so lower the limit until the natural design fits (their energy is mostly low anyway).
    double w_cap = 0;
    int natural = 0;
    for (double cap_hz : {10000.0, 7000.0, 5000.0, 3500.0}) {
      w_cap = 2 * kPi * std::min(cap_hz, 0.45 * sr) / sr;
      if (w_cap <= 4 * kPi * f0 / sr) break;
      LoopDesign probe = {};
      if (AbelSmith(sr, f0, B, loss_a, w_cap, 1.0, probe)) { natural = probe.biquads; break; }
      // AbelSmith refuses designs over the section limit; try a lower cap
    }
    for (int extra_mul = 0; natural > 0 && extra_mul <= 2; ++extra_mul) {
      const int extra = natural * extra_mul;
      if (natural + extra > kMaxDispersionBiquads) break;
      auto build = [&](double alpha, Candidate& out) {
        LoopDesign base = {};
        base.loss_a = float(loss_a);
        if (!AbelSmith(sr, f0, B, loss_a, w_cap, alpha, base, extra)) return false;
        out = Complete(sr, f0, base);
        return out.feasible;
      };
      auto err = [&](double alpha) {
        Candidate c;
        return build(alpha, c) ? PartialError(c.design, sr, f0, B, true) : 1e9;
      };
      double lo = 0.3, hi = 3.0;
      const double phi = 0.6180339887;
      double x1 = hi - phi * (hi - lo), x2 = lo + phi * (hi - lo);
      double e1 = err(x1), e2 = err(x2);
      if (e1 >= 1e9 && e2 >= 1e9) continue;
      for (int it = 0; it < 10; ++it) {
        if (e1 < e2) { hi = x2; x2 = x1; e2 = e1; x1 = hi - phi * (hi - lo); e1 = err(x1); }
        else { lo = x1; x1 = x2; e1 = e2; x2 = lo + phi * (hi - lo); e2 = err(x2); }
      }
      Candidate c;
      if (build(e1 < e2 ? x1 : x2, c))
        options.push_back({c, PartialError(c.design, sr, f0, B, true), 3 * c.design.biquads});
    }
  }
  if (B > 1e-7) {
    for (int stages : {2, 4, 8, 12, 16}) {
      if (stages > limit) break;
      double lo = -0.95, hi = 0.0;
      const double phi = 0.6180339887;
      auto err = [&](double c) {
        Candidate cand = Complete(sr, f0, loss_a, c, stages);
        return cand.feasible ? PartialError(cand.design, sr, f0, B, true) : 1e9;
      };
      double x1 = hi - phi * (hi - lo), x2 = lo + phi * (hi - lo);
      double e1 = err(x1), e2 = err(x2);
      for (int it = 0; it < 30; ++it) {
        if (e1 < e2) { hi = x2; x2 = x1; e2 = e1; x1 = hi - phi * (hi - lo); e1 = err(x1); }
        else { lo = x1; x1 = x2; e1 = e2; x2 = lo + phi * (hi - lo); e2 = err(x2); }
      }
      double c = e1 < e2 ? x1 : x2;
      Candidate cand = Complete(sr, f0, loss_a, c, stages);
      if (cand.feasible) options.push_back({cand, PartialError(cand.design, sr, f0, B, true), 2 * stages});
    }
  }
  std::stable_sort(options.begin(), options.end(), [](const Option& a, const Option& b) { return a.cost < b.cost; });
  double best_rms = 1e9;
  for (auto& o : options) best_rms = std::min(best_rms, o.rms);
  Candidate chosen = options[0].cand;
  for (auto& o : options) {  // cheapest that is within ~1 cent of the best (or simply good)
    if (o.rms <= std::max({best_rms * 1.2, best_rms + 1.0, 1.5})) { chosen = o.cand; break; }
  }
  chosen.design.loss_g = float(std::min(g, 0.99999));
  chosen.design.max_partial_error_cents = PartialError(chosen.design, sr, f0, B);
  chosen.design.rms_partial_error_cents = PartialError(chosen.design, sr, f0, B, true);
  return chosen.design;
}

LoopDesign DesignLoopLike(const LoopDesign& like, double sr, const LoopParams& p) {
  double g, loss_a;
  FitLoss(sr, p.f0_hz, std::max(0.0, p.b1), std::max(0.0, p.b3), g, loss_a);
  LoopDesign base = like;
  base.loss_a = float(loss_a);
  Candidate c = Complete(sr, p.f0_hz, base);
  if (!c.feasible) return like;
  c.design.loss_g = float(std::min(g, 0.99999));
  return c.design;
}

LoopDesign RetuneLoop(const LoopDesign& d, double sr, double f0) {
  Candidate c = Complete(sr, f0, d);
  if (!c.feasible) return d;
  c.design.loss_g = d.loss_g;
  c.design.max_partial_error_cents = d.max_partial_error_cents;
  c.design.rms_partial_error_cents = d.rms_partial_error_cents;
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
  memset(bq_s1_, 0, sizeof(bq_s1_));
  memset(bq_s2_, 0, sizeof(bq_s2_));
}

}
}
}
}
