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
#define NOMINMAX  // std::min/std::max, not the windows.h macros
#include <algorithm>
#include <atomic>
#include <thread>
#include "../../../dsp.h"

namespace dsp {
namespace audio {
namespace synth {
namespace instruments {

namespace {

const double kPi = 3.14159265358979323846;

double NoteHz(int midi) { return 440.0 * pow(2.0, (midi - 69) / 12.0); }

// Median of three neighbours: removes single-note outliers (a notch or beating spoiling one fit).
std::vector<double> Median3(const std::vector<double>& v) {
  std::vector<double> out(v);
  for (size_t i = 1; i + 1 < v.size(); ++i) {
    double a = v[i - 1], b = v[i], c = v[i + 1];
    out[i] = std::max(std::min(a, b), std::min(std::max(a, b), c));
  }
  return out;
}

// Linear interpolation of v (one value per entry of xs, sorted) at x, clamped at the ends.
double Interp(const std::vector<double>& xs, const std::vector<double>& v, double x) {
  if (xs.empty()) return 0;
  if (x <= xs.front()) return v.front();
  if (x >= xs.back()) return v.back();
  size_t i = 0;
  while (i + 1 < xs.size() && xs[i + 1] < x) ++i;
  double t = (x - xs[i]) / (xs[i + 1] - xs[i]);
  return v[i] + t * (v[i + 1] - v[i]);
}

// Resamples `src` (at src_rate) to dst_rate while transposing by `pitch_ratio` (>1 = higher),
// with a Hann-windowed sinc that also low-passes when the step exceeds one source sample.
std::vector<float> Resample(const std::vector<float>& src, double src_rate, double dst_rate, double pitch_ratio) {
  const double step = src_rate / dst_rate * pitch_ratio;  // source samples per output sample
  const double cutoff = std::min(1.0, 1.0 / step);
  const int half = 16;                                    // zero crossings each side (at cutoff)
  const double reach = half / cutoff;
  const size_t out_len = size_t(src.size() / step);
  std::vector<float> out(out_len);
  for (size_t k = 0; k < out_len; ++k) {
    double t = k * step;
    int first = int(ceil(t - reach)), last = int(floor(t + reach));
    double acc = 0;
    for (int i = std::max(0, first); i <= last && i < int(src.size()); ++i) {
      double x = (t - i) * cutoff;
      double sinc = fabs(x) < 1e-9 ? 1.0 : sin(kPi * x) / (kPi * x);
      double w = 0.5 + 0.5 * cos(kPi * (t - i) / reach);
      acc += src[i] * sinc * w * cutoff;
    }
    out[k] = float(acc);
  }
  return out;
}

}  // namespace

CalibratedString::CalibratedString(std::shared_ptr<const StringCalibration> calibration,
                                   const CalibratedStringOptions& options)
    : InstrumentProcessor(), cal_(std::move(calibration)), options_(options), gain_(1.0f), layered_(false),
      voices_(nullptr), dc_coef_(0.997f), level_decay_(0.9995f), cdata_(nullptr) {
  memset(designs_, 0, sizeof(designs_));
  memset(designs_b_, 0, sizeof(designs_b_));
  memset(params_, 0, sizeof(params_));
  memset(after_level_, 0, sizeof(after_level_));
  for (float& r : tuning_ratio_) r = 1.0f;
  for (const auto& n : cal_->notes)
    if (!n.layers.empty()) layered_ = true;
}

CalibratedString::~CalibratedString() {
  Unload();
}

void CalibratedString::BuildTables() {
  const auto& notes = cal_->notes;
  const size_t n = notes.size();
  std::vector<double> midi(n), log_b1(n), log_b3(n), loud(n), tuning(n);
  std::vector<double> b_midi, log_b;       // only notes where B could be measured
  std::vector<double> after_midi, log_ab1, log_ab3, after_lvl;
  std::vector<double> all_after(n, 0.0);
  for (size_t i = 0; i < n; ++i) {
    const CalibratedNote& c = notes[i];
    midi[i] = c.midi_note;
    tuning[i] = c.f0_hz > 0 ? 1200.0 * log2(c.f0_hz / NoteHz(c.midi_note)) : 0.0;
    if (c.inharmonicity > 0) {
      b_midi.push_back(c.midi_note);
      log_b.push_back(log(c.inharmonicity));
    }
    log_b1[i] = log(std::max(0.005f, c.b1));
    log_b3[i] = log(c.b3 + 1e-10);
    loud[i] = c.layers.empty() ? c.peak : c.layers.back().peak;  // loudest layer
    all_after[i] = c.after_level;
    if (c.after_level > 0) {
      after_midi.push_back(c.midi_note);
      log_ab1.push_back(log(std::max(0.005f, c.after_b1)));
      log_ab3.push_back(log(c.after_b3 + 1e-10));
    }
  }
  log_b = Median3(log_b);
  log_b1 = Median3(log_b1);
  log_b3 = Median3(log_b3);
  log_ab1 = Median3(log_ab1);
  log_ab3 = Median3(log_ab3);
  all_after = Median3(all_after);
  tuning = Median3(tuning);

  // Each note only writes its own table entries, so the (slow) loop design runs on all cores.
  auto build_note = [&](int m) {
    tuning_ratio_[m] = options_.recorded_tuning ? float(pow(2.0, Interp(midi, tuning, m) / 1200.0)) : 1.0f;
    LoopParams p;
    p.f0_hz = NoteHz(m) * tuning_ratio_[m];
    p.inharmonicity = b_midi.empty() ? 0.0 : exp(Interp(b_midi, log_b, m));
    p.b1 = exp(Interp(midi, log_b1, m));
    p.b3 = std::max(0.0, exp(Interp(midi, log_b3, m)) - 1e-10);
    params_[m] = p;
    const int stages = DispersionStagesFor(p.f0_hz);
    designs_[m] = DesignLoop(sample_rate_, p, stages);

    // Second string: aftersound decay, slightly detuned, same stiffness (reuses the dispersion).
    after_level_[m] = float(std::max(0.0, Interp(midi, all_after, m)));
    if (after_level_[m] > 0.01f && !after_midi.empty()) {
      LoopParams pb = p;
      pb.f0_hz = p.f0_hz * pow(2.0, options_.detune_cents / 1200.0);
      pb.b1 = exp(Interp(after_midi, log_ab1, m));
      pb.b3 = std::max(0.0, exp(Interp(after_midi, log_ab3, m)) - 1e-10);
      designs_b_[m] = DesignLoopLike(designs_[m], sample_rate_, pb);
    } else {
      after_level_[m] = 0;
    }

    // Excitation layers from the nearest recorded note, transposed to this note.
    size_t best = 0;
    for (size_t i = 1; i < n; ++i)
      if (fabs(midi[i] - m) < fabs(midi[best] - m)) best = i;
    const CalibratedNote& src = notes[best];
    const double ratio = p.f0_hz / src.f0_hz;
    layers_[m].clear();
    if (src.layers.empty()) {
      layers_[m].push_back({1.0f, Resample(src.excitation, src.excitation_rate, sample_rate_, ratio)});
    } else {
      for (const ExcitationLayer& l : src.layers)
        layers_[m].push_back({l.velocity, Resample(l.samples, src.excitation_rate, sample_rate_, ratio)});
    }
  };
  std::atomic<int> next(0);
  auto worker = [&]() {
    for (int m; (m = next.fetch_add(1)) < 128;) build_note(m);
  };
  const unsigned threads = std::max(1u, std::min(16u, std::thread::hardware_concurrency()));
  std::vector<std::thread> pool;
  for (unsigned t = 1; t < threads; ++t) pool.emplace_back(worker);
  worker();
  for (auto& t : pool) t.join();

  std::sort(loud.begin(), loud.end());
  double median_peak = loud[n / 2];
  gain_ = float(options_.level / std::max(1e-6, median_peak));
}

int CalibratedString::Load() {
  if (loaded_)
    return S_FALSE;
  if (!cal_ || cal_->notes.empty())
    return S_FALSE;
  BuildTables();
  voices_ = new Voice[Polyphony];
  bool any_second = false;
  for (int m = 0; m < 128; ++m) any_second |= after_level_[m] > 0;
  for (int v = 0; v < Polyphony; ++v) {
    Voice& voice = voices_[v];
    voice.loop.Allocate(int(sample_rate_ / 8) + 64);
    if (any_second) voice.loop_b.Allocate(int(sample_rate_ / 8) + 64);
    voice.exc_lo = voice.exc_hi = nullptr;
    voice.len_lo = voice.len_hi = voice.exc_pos = 0;
    voice.w_lo = voice.w_hi = 0;
    voice.share_b = 0;
    voice.two_strings = false;
    voice.dc_x1 = voice.dc_y1 = 0;
    voice.level = 0;
    voice.base_gain = voice.base_gain_b = 0;
    voice.note = 0;
    voice.freq = 0;
    voice.active = voice.released = false;
  }
  dc_coef_ = float(1.0 - 2.0 * kPi * 20.0 / sample_rate_);
  level_decay_ = float(exp(-1.0 / (0.05 * sample_rate_)));
  loaded_ = true;
  return S_OK;
}

int CalibratedString::Unload() {
  if (!loaded_)
    return S_FALSE;
  delete[] voices_;
  voices_ = nullptr;
  for (auto& l : layers_) std::vector<Layer>().swap(l);
  loaded_ = false;
  return S_OK;
}

LoopParams CalibratedString::NoteParams(int midi_note) const {
  return params_[std::min(127, std::max(0, midi_note))];
}

int CalibratedString::SetFrequency(real_t freq, int v) {
  auto& nd = cdata_->note_data_array[v];
  nd.freq = nd.base_freq = freq;
  voices_[v].freq = freq;
  return S_OK;
}

int CalibratedString::NoteOn(int v) {
  Voice& voice = voices_[v];
  const auto& nd = cdata_->note_data_array[v];
  const int note = std::min(127, std::max(0, nd.note));
  const bool bent = voice.freq > 0 && fabs(voice.freq / NoteHz(note) - 1.0) > 1e-4;
  const double target = (voice.freq > 0 ? voice.freq : NoteHz(note)) * tuning_ratio_[note];

  LoopDesign design = designs_[note];
  if (bent) design = RetuneLoop(design, sample_rate_, target);  // started under pitch bend
  voice.loop.SetDesign(design);
  voice.loop.Reset();
  voice.base_gain = design.loss_g;

  voice.two_strings = after_level_[note] > 0;
  voice.share_b = after_level_[note];
  if (voice.two_strings) {
    LoopDesign db = designs_b_[note];
    if (bent) db = RetuneLoop(db, sample_rate_, target * pow(2.0, options_.detune_cents / 1200.0));
    voice.loop_b.SetDesign(db);
    voice.loop_b.Reset();
    voice.base_gain_b = db.loss_g;
  }

  // Velocity: crossfade the two layers around it; below the softest layer, fade it down.
  const std::vector<Layer>& layers = layers_[note];
  const float vel = layered_ ? std::max(0.0f, std::min(1.0f, float(nd.velocity))) : 1.0f;
  size_t hi = 0;
  while (hi < layers.size() && layers[hi].velocity < vel) ++hi;
  if (hi == 0) {
    float lowest = layers[0].velocity;
    float t = lowest > 0 ? vel / lowest : 1.0f;  // 0..1 below the softest layer
    float db = (1.0f - t) * options_.min_velocity_gain_db;
    voice.exc_lo = voice.exc_hi = layers[0].samples.data();
    voice.len_lo = voice.len_hi = int(layers[0].samples.size());
    voice.w_lo = powf(10.0f, db / 20.0f);
    voice.w_hi = 0;
  } else if (hi == layers.size()) {
    const Layer& top = layers.back();
    voice.exc_lo = voice.exc_hi = top.samples.data();
    voice.len_lo = voice.len_hi = int(top.samples.size());
    voice.w_lo = 1.0f;
    voice.w_hi = 0;
  } else {
    const Layer& a = layers[hi - 1];
    const Layer& b = layers[hi];
    float t = (vel - a.velocity) / std::max(1e-6f, b.velocity - a.velocity);
    voice.exc_lo = a.samples.data();
    voice.len_lo = int(a.samples.size());
    voice.exc_hi = b.samples.data();
    voice.len_hi = int(b.samples.size());
    voice.w_lo = 1.0f - t;
    voice.w_hi = t;
  }
  voice.w_lo *= gain_;
  voice.w_hi *= gain_;
  voice.exc_pos = 0;
  voice.dc_x1 = voice.dc_y1 = 0;
  voice.level = 1.0f;
  voice.note = note;
  voice.released = false;
  voice.active = true;
  return S_OK;
}

int CalibratedString::NoteOff(int v) {
  Voice& voice = voices_[v];
  if (!voice.active)
    return S_OK;
  voice.released = true;
  if (voice.note >= options_.undamped_from_note)
    return S_OK;  // no damper on this string: it rings on
  const double f = voice.freq > 0 ? voice.freq : NoteHz(voice.note);
  const float damp = ReleaseGain(f, options_.release_t60_s);
  voice.loop.SetLossGain(voice.base_gain * damp);
  if (voice.two_strings) voice.loop_b.SetLossGain(voice.base_gain_b * damp);
  return S_OK;
}

void CalibratedString::BendFrequency(real_t freq, int v) {
  Voice& voice = voices_[v];
  voice.freq = freq;
  if (!voice.active)
    return;
  const double target = freq * tuning_ratio_[voice.note];
  voice.loop.SetDesign(RetuneLoop(voice.loop.design(), sample_rate_, target));
  if (voice.two_strings)
    voice.loop_b.SetDesign(RetuneLoop(voice.loop_b.design(), sample_rate_, target * pow(2.0, options_.detune_cents / 1200.0)));
}

real_t CalibratedString::Tick(int v) {
  Voice& voice = voices_[v];
  if (!voice.active)
    return 0;
  float exc = 0;
  const int pos = voice.exc_pos;
  if (pos < voice.len_lo) exc += voice.exc_lo[pos] * voice.w_lo;
  if (voice.w_hi != 0 && pos < voice.len_hi) exc += voice.exc_hi[pos] * voice.w_hi;
  const bool exciting = pos < std::max(voice.len_lo, voice.len_hi);
  if (exciting) ++voice.exc_pos;

  float out;
  if (voice.two_strings)
    out = voice.loop.Tick(exc * (1.0f - voice.share_b)) + voice.loop_b.Tick(exc * voice.share_b);
  else
    out = voice.loop.Tick(exc);

  float o = out - voice.dc_x1 + dc_coef_ * voice.dc_y1;
  voice.dc_x1 = out; voice.dc_y1 = o;
  float mag = fabsf(o);
  voice.level = mag > voice.level ? mag : voice.level * level_decay_;
  if (voice.level < 1e-5f && !exciting)
    voice.active = false;
  return o;
}

}
}
}
}
