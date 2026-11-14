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
#include <complex>
#include "../../../dsp.h"

namespace dsp {
namespace audio {
namespace synth {
namespace instruments {

namespace {

const double kPi = 3.14159265358979323846;

// Phase delay (samples) at radian frequency w of the loop low-pass (1-S) + S z^-1.
double LoopFilterDelay(double S, double w) {
  std::complex<double> h = (1.0 - S) + S * std::polar(1.0, -w);
  return -std::arg(h) / w;
}
double LoopFilterGain(double S, double w) {
  return std::abs((1.0 - S) + S * std::polar(1.0, -w));
}
// Phase delay at w of the first-order allpass (a + z^-1) / (1 + a z^-1).
double AllpassDelay(double a, double w) {
  std::complex<double> z1 = std::polar(1.0, -w);
  return -std::arg((a + z1) / (1.0 + a * z1)) / w;
}
// Allpass coefficient whose phase delay at w equals `delay` (delay decreases as a grows).
double SolveAllpass(double delay, double w) {
  double lo = -0.999, hi = 0.999;
  for (int i = 0; i < 40; ++i) {
    double mid = 0.5 * (lo + hi);
    if (AllpassDelay(mid, w) > delay) lo = mid; else hi = mid;
  }
  return 0.5 * (lo + hi);
}

}  // namespace

WaveguideString::WaveguideString(const WaveguideParams& params)
    : InstrumentProcessor(), params_(params), voices_(nullptr), line_capacity_(0), stiffness_coef_(0),
      dc_coef_(0.997f), level_decay_(0.9995f), seed_(0x1234567), cdata_(nullptr) {
  if (params_.strings < 1) params_.strings = 1;
  if (params_.strings > kMaxStrings) params_.strings = kMaxStrings;
  if (params_.stiffness_stages > kMaxStiffness) params_.stiffness_stages = kMaxStiffness;
  if (params_.stiffness == 0) params_.stiffness_stages = 0;
}

WaveguideString::~WaveguideString() {
  Unload();
}

int WaveguideString::Load() {
  if (loaded_)
    return S_FALSE;
  line_capacity_ = int(sample_rate_ / 8) + 16;  // lowest pitch ~8 Hz (MIDI note 0)
  voices_ = new Voice[Polyphony];
  memset(voices_, 0, sizeof(Voice) * Polyphony);
  for (int v = 0; v < Polyphony; ++v) {
    for (int s = 0; s < kMaxStrings; ++s) {
      voices_[v].strings[s].line = new float[line_capacity_];
      memset(voices_[v].strings[s].line, 0, sizeof(float) * line_capacity_);
    }
  }
  stiffness_coef_ = float(params_.stiffness);
  dc_coef_ = float(1.0 - 2.0 * kPi * 20.0 / sample_rate_);         // ~20 Hz DC blocker
  level_decay_ = float(exp(-1.0 / (0.05 * sample_rate_)));         // 50 ms peak follower
  loaded_ = true;
  return S_OK;
}

int WaveguideString::Unload() {
  if (!loaded_)
    return S_FALSE;
  for (int v = 0; v < Polyphony; ++v)
    for (int s = 0; s < kMaxStrings; ++s)
      delete[] voices_[v].strings[s].line;
  delete[] voices_;
  voices_ = nullptr;
  loaded_ = false;
  return S_OK;
}

float WaveguideString::Random() {
  seed_ = seed_ * 1664525u + 1013904223u;
  return float(int32_t(seed_)) * (1.0f / 2147483648.0f);
}

void WaveguideString::Tune(Voice& voice, real_t freq, bool reset_state) {
  const double sr = sample_rate_;
  const double t60 = std::min(60.0, std::max(0.05, params_.t60_c4_s * pow(261.63 / freq, params_.t60_slope)));
  for (int s = 0; s < params_.strings; ++s) {
    String& st = voice.strings[s];
    // Unison strings are spread symmetrically around the note.
    double cents = params_.strings > 1 ? (s == 0 ? -0.5 : 0.5) * params_.detune_cents : 0.0;
    double f = freq * pow(2.0, cents / 1200.0);
    double w = 2.0 * kPi * f / sr;
    double period = sr / f;

    // High notes get a gentler loop filter, otherwise they'd die almost instantly.
    double S = params_.damping * std::min(1.0, sqrt(1000.0 / f));
    double filters = LoopFilterDelay(S, w) + params_.stiffness_stages * AllpassDelay(params_.stiffness, w);
    double remaining = period - filters;
    int delay = int(floor(remaining - 0.5));
    if (delay < 1) delay = 1;
    if (delay > line_capacity_ - 2) delay = line_capacity_ - 2;
    double frac = remaining - delay;  // ~0.5..1.5 samples, realised by the allpass

    st.delay = delay;
    st.frac_coef = float(SolveAllpass(frac, w));
    st.damping = float(S);

    // Loop gain so the fundamental decays 60 dB in t60 (and in release_t60 after key-up).
    double loss = LoopFilterGain(S, w);
    double held = pow(10.0, -3.0 / (f * t60)) / loss;
    double released = pow(10.0, -3.0 / (f * std::max(0.01, double(params_.release_t60_s)))) / loss;
    st.gain_held = float(std::min(held, 0.99995));
    st.gain_released = float(std::min(released, 0.99995));
    st.gain = voice.released ? st.gain_released : st.gain_held;

    if (reset_state) {
      memset(st.line, 0, sizeof(float) * (delay + 4));
      st.write = 0;
      st.frac_x1 = st.frac_y1 = st.lp_x1 = 0;
      memset(st.stiff_x1, 0, sizeof(st.stiff_x1));
      memset(st.stiff_y1, 0, sizeof(st.stiff_y1));
    }
  }
}

void WaveguideString::BuildExcitation(Voice& voice, real_t freq, real_t velocity, real_t amplitude, bool with_body) {
  const double sr = sample_rate_;
  const double period = sr / freq;
  int len = with_body ? kExcitationLength : std::min(kExcitationLength, int(2 * period) + 64);
  float* e = voice.excitation;
  memset(e, 0, sizeof(float) * len);

  // Pluck: a smooth pulse (narrower = brighter, so harder notes are brighter) plus noise.
  double cutoff = params_.pluck_cutoff_hz * (1.0 - params_.velocity_tone * (1.0 - velocity));
  cutoff = std::max(200.0, cutoff);
  int width = int(std::min(std::max(sr / cutoff, 1.0), std::max(1.0, period * 0.5)));
  for (int i = 0; i < width && i < len; ++i)
    e[i] += float((1.0 - params_.noise) * 0.5 * (1.0 - cos(2.0 * kPi * (i + 0.5) / width)));
  int noise_len = std::min(len, int(ceil(period)));
  for (int i = 0; i < noise_len; ++i)
    e[i] += float(params_.noise * Random() * exp(-3.0 * i / noise_len));

  // Pick position: plucking at 1/n of the length cancels every n-th harmonic.
  int m = int(params_.pick_position * period + 0.5);
  if (m >= 1)
    for (int i = len - 1; i >= m; --i)
      e[i] -= e[i - m];

  // Tame the noise with a one-pole low-pass at the pluck cutoff.
  {
    float a = float(exp(-2.0 * kPi * cutoff / sr));
    float y = 0;
    for (int i = 0; i < len; ++i) { y = (1 - a) * e[i] + a * y; e[i] = y; }
  }

  // Commuted body: add the response of a few body/soundboard resonances.
  if (with_body && params_.body_modes > 0) {
    std::vector<float> dry(e, e + len);
    for (int k = 0; k < params_.body_modes; ++k) {
      const BodyMode& mode = params_.body[k];
      double r = exp(-6.907755 / (mode.t60_s * sr));  // ln(1000) = 60 dB
      double c1 = 2.0 * r * cos(2.0 * kPi * mode.freq_hz / sr), c2 = -r * r;
      double norm = (1.0 - r) * mode.gain;
      double y1 = 0, y2 = 0;
      for (int i = 0; i < len; ++i) {
        double y = dry[i] + c1 * y1 + c2 * y2;
        y2 = y1; y1 = y;
        e[i] += float(norm * y);
      }
    }
  }

  // Remove DC and normalise.
  double mean = 0;
  for (int i = 0; i < len; ++i) mean += e[i];
  mean /= len;
  float peak = 1e-9f;
  for (int i = 0; i < len; ++i) { e[i] -= float(mean); peak = std::max(peak, fabsf(e[i])); }
  float scale = float(amplitude) / peak;
  for (int i = 0; i < len; ++i) e[i] *= scale;

  voice.exc_pos = 0;
  voice.exc_len = len;
}

int WaveguideString::SetFrequency(real_t freq, int v) {
  auto& nd = cdata_->note_data_array[v];
  nd.freq = nd.base_freq = freq;
  voices_[v].freq = freq;
  return S_OK;
}

int WaveguideString::NoteOn(int v) {
  Voice& voice = voices_[v];
  const auto& nd = cdata_->note_data_array[v];
  voice.released = false;
  Tune(voice, voice.freq, true);
  BuildExcitation(voice, voice.freq, nd.velocity, 1.0f, true);
  voice.dc_x1 = voice.dc_y1 = 0;
  voice.level = 1.0f;
  voice.active = true;
  return S_OK;
}

int WaveguideString::NoteOff(int v) {
  Voice& voice = voices_[v];
  if (!voice.active)
    return S_OK;
  voice.released = true;
  for (int s = 0; s < params_.strings; ++s)
    voice.strings[s].gain = voice.strings[s].gain_released;
  if (params_.release_noise > 0)
    BuildExcitation(voice, voice.freq, 0.5f, params_.release_noise, false);  // damper / quill return
  return S_OK;
}

void WaveguideString::BendFrequency(real_t freq, int v) {
  Voice& voice = voices_[v];
  voice.freq = freq;
  if (voice.active)
    Tune(voice, freq, false);
}

real_t WaveguideString::Tick(int v) {
  Voice& voice = voices_[v];
  if (!voice.active)
    return 0;

  float exc = 0;
  if (voice.exc_pos < voice.exc_len)
    exc = voice.excitation[voice.exc_pos++];
  const float exc_share = exc / params_.strings;
  const int stages = params_.stiffness_stages;
  const float a = stiffness_coef_;

  float out = 0;
  for (int s = 0; s < params_.strings; ++s) {
    String& st = voice.strings[s];
    int r = st.write - st.delay;
    if (r < 0) r += line_capacity_;
    float x = st.line[r];
    // fractional delay allpass
    float y = st.frac_coef * x + st.frac_x1 - st.frac_coef * st.frac_y1;
    st.frac_x1 = x; st.frac_y1 = y;
    // stiffness (dispersion) allpasses
    for (int k = 0; k < stages; ++k) {
      float z = a * y + st.stiff_x1[k] - a * st.stiff_y1[k];
      st.stiff_x1[k] = y; st.stiff_y1[k] = z;
      y = z;
    }
    // loss: gain * ((1-S) + S z^-1)
    float lp = st.gain * ((1.0f - st.damping) * y + st.damping * st.lp_x1);
    st.lp_x1 = y;
    float in = lp + exc_share;
    st.line[st.write] = in;
    if (++st.write == line_capacity_) st.write = 0;
    out += in;
  }

  // DC blocker, then track the level so silent voices can switch off.
  float o = out - voice.dc_x1 + dc_coef_ * voice.dc_y1;
  voice.dc_x1 = out; voice.dc_y1 = o;
  float mag = fabsf(o);
  voice.level = mag > voice.level ? mag : voice.level * level_decay_;
  if (voice.level < 1e-5f && voice.exc_pos >= voice.exc_len)
    voice.active = false;  // below -100 dB
  return o * params_.gain;
}

// ---- Presets ---------------------------------------------------------------------------------
// Starting points chosen from typical instrument behaviour; tune by ear / with DSP_Render.

WaveguideParams WaveguideString::Harpsichord() {
  WaveguideParams p = {};
  p.t60_c4_s = 7.0f;  p.t60_slope = 0.6f;
  p.damping = 0.06f;                     // bright, long-ringing overtones
  p.pick_position = 0.13f;               // quill close to the nut
  p.pluck_cutoff_hz = 10000.0f; p.velocity_tone = 0.1f; p.noise = 0.35f;
  p.body[0] = {110.0f, 0.12f, 0.6f}; p.body[1] = {260.0f, 0.09f, 0.5f}; p.body[2] = {520.0f, 0.07f, 0.4f};
  p.body[3] = {1150.0f, 0.05f, 0.3f}; p.body[4] = {2400.0f, 0.03f, 0.2f};
  p.body_modes = 5;
  p.strings = 2; p.detune_cents = 1.6f;  // two 8' choirs
  p.release_t60_s = 0.09f; p.release_noise = 0.08f;
  p.gain = 0.55f;
  return p;
}

WaveguideParams WaveguideString::Piano() {
  WaveguideParams p = {};
  p.t60_c4_s = 9.0f;  p.t60_slope = 0.75f;
  p.damping = 0.22f;
  p.stiffness = -0.25f; p.stiffness_stages = 2;  // stretched partials
  p.pick_position = 0.12f;               // hammer strike point
  p.pluck_cutoff_hz = 5000.0f; p.velocity_tone = 0.7f; p.noise = 0.05f;  // felt hammer: soft = dark
  p.body[0] = {90.0f, 0.15f, 0.5f}; p.body[1] = {210.0f, 0.12f, 0.45f}; p.body[2] = {430.0f, 0.1f, 0.4f};
  p.body[3] = {870.0f, 0.07f, 0.3f}; p.body[4] = {1700.0f, 0.05f, 0.25f}; p.body[5] = {3300.0f, 0.03f, 0.15f};
  p.body_modes = 6;
  p.strings = 2; p.detune_cents = 0.9f;  // unison beating -> two-stage decay
  p.release_t60_s = 0.25f; p.release_noise = 0.0f;
  p.gain = 0.6f;
  return p;
}

WaveguideParams WaveguideString::NylonGuitar() {
  WaveguideParams p = {};
  p.t60_c4_s = 3.5f;  p.t60_slope = 0.5f;
  p.damping = 0.3f;                      // warm
  p.pick_position = 0.2f;
  p.pluck_cutoff_hz = 4000.0f; p.velocity_tone = 0.5f; p.noise = 0.15f;
  p.body[0] = {98.0f, 0.18f, 0.9f}; p.body[1] = {204.0f, 0.12f, 0.7f}; p.body[2] = {390.0f, 0.08f, 0.45f};
  p.body[3] = {820.0f, 0.05f, 0.25f};
  p.body_modes = 4;
  p.strings = 1;
  p.release_t60_s = 0.15f; p.release_noise = 0.02f;
  p.gain = 0.7f;
  return p;
}

WaveguideParams WaveguideString::SteelGuitar() {
  WaveguideParams p = NylonGuitar();
  p.t60_c4_s = 5.0f;
  p.damping = 0.14f;                     // brighter, rings longer
  p.pick_position = 0.15f;
  p.pluck_cutoff_hz = 7000.0f; p.noise = 0.25f;
  p.gain = 0.6f;
  return p;
}

WaveguideParams WaveguideString::MutedGuitar() {
  WaveguideParams p = NylonGuitar();
  p.t60_c4_s = 0.25f; p.t60_slope = 0.2f;  // palm-muted / pizzicato
  p.damping = 0.35f;
  p.release_t60_s = 0.08f;
  return p;
}

WaveguideParams WaveguideString::Bass() {
  WaveguideParams p = {};
  p.t60_c4_s = 3.0f;  p.t60_slope = 0.6f;
  p.damping = 0.35f;
  p.pick_position = 0.22f;
  p.pluck_cutoff_hz = 2500.0f; p.velocity_tone = 0.5f; p.noise = 0.1f;
  p.body[0] = {75.0f, 0.12f, 0.5f}; p.body[1] = {160.0f, 0.08f, 0.3f};
  p.body_modes = 2;
  p.strings = 1;
  p.release_t60_s = 0.12f; p.release_noise = 0.02f;
  p.gain = 0.8f;
  return p;
}

WaveguideParams WaveguideString::Harp() {
  WaveguideParams p = {};
  p.t60_c4_s = 6.0f;  p.t60_slope = 0.6f;
  p.damping = 0.16f;
  p.pick_position = 0.3f;                // plucked near the middle: round tone
  p.pluck_cutoff_hz = 5000.0f; p.velocity_tone = 0.4f; p.noise = 0.1f;
  p.body[0] = {140.0f, 0.15f, 0.6f}; p.body[1] = {330.0f, 0.1f, 0.4f}; p.body[2] = {700.0f, 0.06f, 0.25f};
  p.body_modes = 3;
  p.strings = 1;
  p.release_t60_s = 3.0f; p.release_noise = 0.0f;  // harp strings aren't damped on release
  p.gain = 0.6f;
  return p;
}

}
}
}
}
