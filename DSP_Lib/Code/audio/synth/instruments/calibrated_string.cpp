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

CalibratedString::CalibratedString(std::shared_ptr<const StringCalibration> calibration, real_t release_t60_s,
                                   real_t level)
    : InstrumentProcessor(), cal_(std::move(calibration)), release_t60_s_(release_t60_s), level_(level), gain_(1.0f),
      voices_(nullptr), dc_coef_(0.997f), level_decay_(0.9995f), cdata_(nullptr) {
  memset(designs_, 0, sizeof(designs_));
  memset(params_, 0, sizeof(params_));
}

CalibratedString::~CalibratedString() {
  Unload();
}

void CalibratedString::BuildTables() {
  const auto& notes = cal_->notes;
  const size_t n = notes.size();
  std::vector<double> midi(n), log_b(n), log_b1(n), log_b3(n), peaks(n);
  for (size_t i = 0; i < n; ++i) {
    midi[i] = notes[i].midi_note;
    log_b[i] = log(notes[i].inharmonicity + 1e-9);
    log_b1[i] = log(std::max(0.005f, notes[i].b1));
    log_b3[i] = log(notes[i].b3 + 1e-10);
    peaks[i] = notes[i].peak;
  }
  log_b = Median3(log_b);
  log_b1 = Median3(log_b1);
  log_b3 = Median3(log_b3);

  auto interp = [&](const std::vector<double>& v, int m) {
    if (m <= midi.front()) return v.front();
    if (m >= midi.back()) return v.back();
    size_t i = 0;
    while (i + 1 < n && midi[i + 1] < m) ++i;
    double t = (m - midi[i]) / (midi[i + 1] - midi[i]);
    return v[i] + t * (v[i + 1] - v[i]);
  };

  for (int m = 0; m < 128; ++m) {
    LoopParams p;
    p.f0_hz = NoteHz(m);
    p.inharmonicity = std::max(0.0, exp(interp(log_b, m)) - 1e-9);
    p.b1 = exp(interp(log_b1, m));
    p.b3 = std::max(0.0, exp(interp(log_b3, m)) - 1e-10);
    params_[m] = p;
    designs_[m] = DesignLoop(sample_rate_, p, DispersionStagesFor(p.f0_hz));

    // Excitation from the nearest recorded note, transposed to this note.
    size_t best = 0;
    for (size_t i = 1; i < n; ++i)
      if (fabs(midi[i] - m) < fabs(midi[best] - m)) best = i;
    const CalibratedNote& src = notes[best];
    excitations_[m] = Resample(src.excitation, src.excitation_rate, sample_rate_, p.f0_hz / src.f0_hz);
  }

  std::sort(peaks.begin(), peaks.end());
  double median_peak = peaks[n / 2];
  gain_ = float(level_ / std::max(1e-6, median_peak));
}

int CalibratedString::Load() {
  if (loaded_)
    return S_FALSE;
  if (!cal_ || cal_->notes.empty())
    return S_FALSE;
  BuildTables();
  voices_ = new Voice[Polyphony];
  for (int v = 0; v < Polyphony; ++v) {
    Voice& voice = voices_[v];
    voice.loop.Allocate(int(sample_rate_ / 8) + 64);
    voice.excitation = nullptr;
    voice.exc_len = voice.exc_pos = 0;
    voice.dc_x1 = voice.dc_y1 = 0;
    voice.level = 0;
    voice.base_gain = 0;
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
  for (auto& e : excitations_) std::vector<float>().swap(e);
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
  LoopDesign design = designs_[note];
  if (voice.freq > 0 && fabs(voice.freq / NoteHz(note) - 1.0) > 1e-4)
    design = RetuneLoop(design, sample_rate_, voice.freq);  // started under pitch bend
  voice.loop.SetDesign(design);
  voice.loop.Reset();
  voice.base_gain = design.loss_g;
  voice.excitation = excitations_[note].data();
  voice.exc_len = int(excitations_[note].size());
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
  voice.loop.SetLossGain(voice.base_gain * ReleaseGain(voice.freq > 0 ? voice.freq : NoteHz(voice.note), release_t60_s_));
  return S_OK;
}

void CalibratedString::BendFrequency(real_t freq, int v) {
  Voice& voice = voices_[v];
  voice.freq = freq;
  if (voice.active)
    voice.loop.SetDesign(RetuneLoop(voice.loop.design(), sample_rate_, freq));
}

real_t CalibratedString::Tick(int v) {
  Voice& voice = voices_[v];
  if (!voice.active)
    return 0;
  float exc = voice.exc_pos < voice.exc_len ? voice.excitation[voice.exc_pos++] : 0.0f;
  float out = voice.loop.Tick(exc * gain_);
  float o = out - voice.dc_x1 + dc_coef_ * voice.dc_y1;
  voice.dc_x1 = out; voice.dc_y1 = o;
  float mag = fabsf(o);
  voice.level = mag > voice.level ? mag : voice.level * level_decay_;
  if (voice.level < 1e-5f && voice.exc_pos >= voice.exc_len)
    voice.active = false;
  return o;
}

}
}
}
}
