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
#include <initializer_list>
#include "../../../dsp.h"

namespace dsp {
namespace audio {
namespace synth {
namespace instruments {

namespace {

const double kPi = 3.14159265358979323846;
const int kKitFirstNote = 27, kKitLastNote = 87;
const float kLn1000 = 6.907755f;  // 60 dB

// ---- small builders for sound definitions ----------------------------------------------------
ModalSound Sound(std::initializer_list<Mode> modes, real_t gain) {
  ModalSound s = {};
  for (const Mode& m : modes)
    if (s.mode_count < ModalSound::kMaxModes) s.modes[s.mode_count++] = m;
  s.mallet_cutoff_hz = 20000.0f;
  s.noise_q = 0.7f;
  s.gain = gain;
  return s;
}
ModalSound Noise(ModalSound s, real_t amount, real_t hz, real_t q, real_t t60) {
  s.noise = amount; s.noise_hz = hz; s.noise_q = q; s.noise_t60 = t60;
  return s;
}
ModalSound Click(ModalSound s, real_t amount) { s.click = amount; return s; }
ModalSound Sweep(ModalSound s, real_t amount, real_t ms) { s.sweep = amount; s.sweep_ms = ms; return s; }
ModalSound Choke(ModalSound s, int group) { s.choke_group = group; return s; }
ModalSound Mallet(ModalSound s, real_t cutoff_hz, real_t attack_ms) {
  s.mallet_cutoff_hz = cutoff_hz; s.attack_ms = attack_ms;
  return s;
}

// Drumhead: ideal circular membrane mode ratios, higher modes quieter and shorter.
ModalSound Membrane(real_t f, real_t t60, real_t gain, int count = 6) {
  static const real_t ratio[] = {1.0f, 1.594f, 2.136f, 2.296f, 2.653f, 2.918f, 3.156f, 3.501f};
  static const real_t level[] = {1.0f, 0.55f, 0.35f, 0.3f, 0.2f, 0.15f, 0.1f, 0.08f};
  ModalSound s = Sound({}, gain);
  for (int i = 0; i < count && i < 8; ++i)
    s.modes[s.mode_count++] = {f * ratio[i], level[i], t60 * (i == 0 ? 1.0f : 0.6f / (1 + 0.3f * i))};
  return s;
}
// Cymbal / hi-hat: a dense cluster of inharmonic modes scaled from `base`.
ModalSound Metal(real_t base, real_t t60, real_t gain) {
  static const real_t ratio[] = {1.0f, 1.483f, 1.932f, 2.546f, 2.630f, 3.897f, 4.405f, 5.142f,
                                 5.779f, 6.637f, 7.452f, 8.317f, 9.381f, 10.48f};
  ModalSound s = Sound({}, gain);
  for (int i = 0; i < 14; ++i)
    s.modes[s.mode_count++] = {base * ratio[i], 0.5f + 0.5f * float((i * 7) % 5) / 4.0f, t60 * (1.0f - 0.03f * i)};
  return s;
}
// Bell-like bar or plate: free-free bar ratios.
ModalSound Bar(real_t f, real_t t60, real_t gain) {
  return Sound({{f, 1.0f, t60}, {f * 2.756f, 0.5f, t60 * 0.6f}, {f * 5.404f, 0.25f, t60 * 0.4f},
                {f * 8.933f, 0.12f, t60 * 0.3f}}, gain);
}

struct KitTable {
  ModalSound sounds[kKitLastNote - kKitFirstNote + 1];
  const char* names[kKitLastNote - kKitFirstNote + 1];
  void Set(int note, const char* name, const ModalSound& s) {
    sounds[note - kKitFirstNote] = s;
    names[note - kKitFirstNote] = name;
  }
  KitTable() {
    memset(this, 0, sizeof(*this));
    // kicks: low membrane, pitch drops from ~3x, beater click
    ModalSound kick = Click(Noise(Sweep(Sound({{55.0f, 1.0f, 0.45f}, {92.0f, 0.3f, 0.2f}, {145.0f, 0.1f, 0.1f}}, 1.6f),
                                         2.0f, 25.0f), 0.08f, 3000.0f, 0.7f, 0.012f), 0.25f);
    ModalSound kick2 = kick;
    kick2.modes[0].freq = 48.0f; kick2.modes[1].freq = 80.0f;
    Set(35, "Acoustic Bass Drum", kick2);
    Set(36, "Bass Drum 1", kick);

    Set(37, "Side Stick", Click(Noise(Sound({{1750.0f, 1.0f, 0.05f}, {3150.0f, 0.5f, 0.035f}, {560.0f, 0.5f, 0.06f}}, 0.7f),
                                      0.3f, 3000.0f, 1.0f, 0.02f), 0.4f));
    // snares: membrane + bright noise (the wires)
    ModalSound snare = Click(Noise(Sweep(Membrane(190.0f, 0.16f, 0.9f, 5), 0.15f, 8.0f), 1.0f, 4500.0f, 0.6f, 0.22f), 0.3f);
    Set(38, "Acoustic Snare", snare);
    ModalSound snare2 = Click(Noise(Membrane(230.0f, 0.12f, 0.8f, 4), 1.0f, 5500.0f, 0.7f, 0.17f), 0.35f);
    Set(40, "Electric Snare", snare2);
    Set(39, "Hand Clap", Click(Noise(Sound({}, 3.5f), 1.0f, 1200.0f, 1.3f, 0.12f), 0.2f));
    // toms: membranes with a small pitch drop
    const struct { int note; const char* name; real_t f; } toms[] = {
        {41, "Low Floor Tom", 82.0f}, {43, "High Floor Tom", 98.0f}, {45, "Low Tom", 115.0f},
        {47, "Low-Mid Tom", 135.0f}, {48, "Hi-Mid Tom", 155.0f}, {50, "High Tom", 180.0f}};
    for (auto& t : toms)
      Set(t.note, t.name, Click(Noise(Sweep(Membrane(t.f, 0.9f, 0.7f, 5), 0.3f, 40.0f), 0.08f, 1000.0f, 0.7f, 0.03f), 0.2f));
    // hi-hats: metal cluster + noise, all in choke group 1
    Set(42, "Closed Hi-Hat", Choke(Noise(Metal(3150.0f, 0.05f, 0.25f), 1.0f, 9000.0f, 0.5f, 0.045f), 1));
    Set(44, "Pedal Hi-Hat", Choke(Noise(Metal(3150.0f, 0.08f, 0.22f), 0.8f, 8500.0f, 0.5f, 0.07f), 1));
    Set(46, "Open Hi-Hat", Choke(Noise(Metal(3150.0f, 0.8f, 0.25f), 0.9f, 9000.0f, 0.5f, 0.7f), 1));
    // cymbals
    Set(49, "Crash Cymbal 1", Noise(Metal(480.0f, 3.2f, 0.14f), 0.8f, 6000.0f, 0.4f, 2.6f));
    Set(57, "Crash Cymbal 2", Noise(Metal(530.0f, 3.0f, 0.14f), 0.8f, 6500.0f, 0.4f, 2.4f));
    Set(55, "Splash Cymbal", Noise(Metal(700.0f, 1.3f, 0.14f), 0.8f, 7500.0f, 0.4f, 1.0f));
    Set(52, "Chinese Cymbal", Noise(Metal(420.0f, 2.4f, 0.16f), 0.9f, 3500.0f, 0.5f, 2.0f));
    Set(51, "Ride Cymbal 1", Noise(Metal(610.0f, 4.5f, 0.1f), 0.3f, 7000.0f, 0.5f, 3.5f));
    Set(59, "Ride Cymbal 2", Noise(Metal(650.0f, 4.2f, 0.1f), 0.3f, 7500.0f, 0.5f, 3.2f));
    Set(53, "Ride Bell", Noise(Bar(1180.0f, 1.5f, 0.6f), 0.1f, 6000.0f, 0.5f, 0.5f));
    // hand percussion
    Set(54, "Tambourine", Noise(Sound({{6500.0f, 0.4f, 0.25f}, {8100.0f, 0.4f, 0.22f}, {9700.0f, 0.3f, 0.2f}}, 0.75f),
                                0.8f, 9000.0f, 0.6f, 0.2f));
    Set(56, "Cowbell", Click(Sound({{540.0f, 1.0f, 0.3f}, {800.0f, 0.8f, 0.25f}, {1620.0f, 0.15f, 0.1f}}, 0.6f), 0.1f));
    Set(58, "Vibraslap", Noise(Sound({}, 1.2f), 1.0f, 3000.0f, 2.0f, 0.8f));
    Set(60, "Hi Bongo", Click(Noise(Sweep(Membrane(400.0f, 0.18f, 0.8f, 3), 0.05f, 10.0f), 0.1f, 2000.0f, 0.7f, 0.02f), 0.2f));
    Set(61, "Low Bongo", Click(Noise(Sweep(Membrane(300.0f, 0.22f, 0.8f, 3), 0.05f, 10.0f), 0.1f, 2000.0f, 0.7f, 0.02f), 0.2f));
    Set(62, "Mute Hi Conga", Click(Membrane(330.0f, 0.08f, 0.8f, 3), 0.3f));
    Set(63, "Open Hi Conga", Click(Sweep(Membrane(330.0f, 0.3f, 0.8f, 3), 0.05f, 10.0f), 0.2f));
    Set(64, "Low Conga", Click(Sweep(Membrane(220.0f, 0.35f, 0.8f, 3), 0.05f, 10.0f), 0.2f));
    Set(65, "High Timbale", Click(Noise(Sound({{350.0f, 1.0f, 0.4f}, {805.0f, 0.4f, 0.3f}, {1310.0f, 0.3f, 0.25f}}, 0.7f),
                                        0.2f, 3500.0f, 0.7f, 0.1f), 0.3f));
    Set(66, "Low Timbale", Click(Noise(Sound({{250.0f, 1.0f, 0.45f}, {575.0f, 0.4f, 0.3f}, {940.0f, 0.3f, 0.25f}}, 0.7f),
                                       0.2f, 3000.0f, 0.7f, 0.1f), 0.3f));
    ModalSound agogo = Sound({{900.0f, 1.0f, 0.5f}, {2277.0f, 0.5f, 0.3f}, {4140.0f, 0.2f, 0.2f}}, 0.5f);
    Set(67, "High Agogo", agogo);
    agogo.modes[0].freq = 650.0f; agogo.modes[1].freq = 1645.0f; agogo.modes[2].freq = 2990.0f;
    Set(68, "Low Agogo", agogo);
    Set(69, "Cabasa", Noise(Sound({}, 1.0f), 1.0f, 7000.0f, 0.8f, 0.08f));
    Set(70, "Maracas", Noise(Sound({}, 1.0f), 1.0f, 9000.0f, 0.8f, 0.05f));
    Set(71, "Short Whistle", Sound({{2500.0f, 1.0f, 0.15f}}, 0.6f));
    Set(72, "Long Whistle", Sound({{2100.0f, 1.0f, 0.5f}}, 0.6f));
    Set(73, "Short Guiro", Noise(Sound({}, 1.5f), 1.0f, 3000.0f, 2.0f, 0.1f));
    Set(74, "Long Guiro", Noise(Sound({}, 1.5f), 1.0f, 3000.0f, 2.0f, 0.3f));
    Set(75, "Claves", Click(Sound({{2500.0f, 1.0f, 0.08f}, {6900.0f, 0.1f, 0.04f}}, 0.6f), 0.2f));
    Set(76, "Hi Wood Block", Click(Sound({{1300.0f, 1.0f, 0.08f}, {3380.0f, 0.4f, 0.05f}}, 0.6f), 0.3f));
    Set(77, "Low Wood Block", Click(Sound({{900.0f, 1.0f, 0.09f}, {2340.0f, 0.4f, 0.06f}}, 0.6f), 0.3f));
    Set(78, "Mute Cuica", Sweep(Sound({{600.0f, 1.0f, 0.12f}}, 0.4f), -0.4f, 30.0f));
    Set(79, "Open Cuica", Sweep(Sound({{450.0f, 1.0f, 0.3f}}, 0.4f), -0.5f, 60.0f));
    ModalSound triangle = Sound({{2100.0f, 1.0f, 2.5f}, {4400.0f, 0.6f, 2.0f}, {6900.0f, 0.4f, 1.6f}, {9100.0f, 0.3f, 1.2f}}, 0.25f);
    Set(81, "Open Triangle", triangle);
    for (int i = 0; i < triangle.mode_count; ++i) triangle.modes[i].t60 = 0.1f;
    Set(80, "Mute Triangle", triangle);
    // GM2 extras at the edges of the map
    Set(27, "High Q", Sweep(Sound({{900.0f, 1.0f, 0.06f}}, 0.5f), 1.0f, 8.0f));
    Set(28, "Slap", Click(Noise(Sound({}, 0.6f), 1.0f, 2500.0f, 1.0f, 0.04f), 0.4f));
    Set(31, "Sticks", Click(Sound({{2900.0f, 1.0f, 0.05f}, {5100.0f, 0.4f, 0.03f}}, 0.6f), 0.3f));
    Set(32, "Square Click", Click(Sound({}, 1.0f), 0.6f));
    Set(33, "Metronome Click", Click(Sound({{2000.0f, 1.0f, 0.03f}}, 0.5f), 0.3f));
    Set(34, "Metronome Bell", Bar(2600.0f, 0.4f, 0.4f));
    Set(29, "Scratch Push", Noise(Sound({}, 1.5f), 1.0f, 1500.0f, 1.5f, 0.1f));
    Set(30, "Scratch Pull", Noise(Sound({}, 1.5f), 1.0f, 1100.0f, 1.5f, 0.1f));
    Set(82, "Shaker", Noise(Sound({}, 1.0f), 1.0f, 8000.0f, 0.8f, 0.07f));
    Set(83, "Jingle Bell", Noise(Bar(3200.0f, 0.4f, 0.4f), 0.6f, 9000.0f, 0.6f, 0.3f));
    Set(84, "Bell Tree", Bar(2400.0f, 1.5f, 0.3f));
    Set(85, "Castanets", Click(Sound({{1900.0f, 1.0f, 0.04f}, {4300.0f, 0.4f, 0.03f}}, 0.6f), 0.3f));
    Set(86, "Mute Surdo", Click(Membrane(70.0f, 0.15f, 1.0f, 3), 0.3f));
    Set(87, "Open Surdo", Click(Sweep(Membrane(70.0f, 0.6f, 1.0f, 3), 0.1f, 20.0f), 0.2f));
  }
};

const KitTable& Kit() {
  static const KitTable table;
  return table;
}

}  // namespace

ModalInstrument::ModalInstrument()
    : InstrumentProcessor(), kit_(false), params_(), kit_sounds_(nullptr), voices_(nullptr), seed_(0x2545F491),
      level_decay_(0.9995f), cdata_(nullptr) {
}

ModalInstrument::ModalInstrument(const ModalParams& params) : ModalInstrument() {
  params_ = params;
}

ModalInstrument* ModalInstrument::NewDrumKit() {
  auto kit = new ModalInstrument();
  kit->kit_ = true;
  kit->kit_sounds_ = Kit().sounds;
  return kit;
}

const char* ModalInstrument::DrumName(int note) {
  if (note < kKitFirstNote || note > kKitLastNote) return nullptr;
  return Kit().names[note - kKitFirstNote];
}

ModalInstrument::~ModalInstrument() {
  Unload();
}

int ModalInstrument::Load() {
  if (loaded_)
    return S_FALSE;
  voices_ = new Voice[Polyphony];
  memset(voices_, 0, sizeof(Voice) * Polyphony);
  level_decay_ = float(exp(-1.0 / (0.05 * sample_rate_)));
  loaded_ = true;
  return S_OK;
}

int ModalInstrument::Unload() {
  if (!loaded_)
    return S_FALSE;
  delete[] voices_;
  voices_ = nullptr;
  loaded_ = false;
  return S_OK;
}

float ModalInstrument::Random() {
  seed_ = seed_ * 1664525u + 1013904223u;
  return float(int32_t(seed_)) * (1.0f / 2147483648.0f);
}

int ModalInstrument::SetFrequency(real_t freq, int v) {
  auto& nd = cdata_->note_data_array[v];
  nd.freq = nd.base_freq = freq;
  voices_[v].freq = freq;
  return S_OK;
}

void ModalInstrument::UpdateRotation(Voice& voice, float multiplier) {
  const float nyquist_w = float(kPi * 0.95);
  for (int k = 0; k < voice.mode_count; ++k) {
    VoiceMode& m = voice.modes[k];
    float w = std::min(m.w * multiplier, nyquist_w);
    m.c = m.r * cosf(w);
    m.s = m.r * sinf(w);
  }
}

void ModalInstrument::Start(Voice& voice, const ModalSound& sound, real_t f0, real_t velocity, bool pitched) {
  const double sr = sample_rate_;
  // Fundamental decay for pitched instruments; drums give absolute T60s.
  double note_t60 = 1.0;
  if (pitched)
    note_t60 = std::min(30.0, std::max(0.02, params_.t60_c4_s * pow(261.63 / f0, params_.t60_slope)));
  // Harder hits reach higher modes.
  const double cutoff = sound.mallet_cutoff_hz * (0.35 + 0.65 * velocity);

  voice.mode_count = 0;
  for (int k = 0; k < sound.mode_count; ++k) {
    const Mode& mode = sound.modes[k];
    double f = pitched ? mode.freq * f0 : mode.freq;
    if (f >= 0.45 * sr || f <= 0)
      continue;
    double t60 = pitched ? mode.t60 * note_t60 : mode.t60;
    double weight = 1.0 / (1.0 + (f / cutoff) * (f / cutoff));
    VoiceMode& m = voice.modes[voice.mode_count++];
    m.w = float(2.0 * kPi * f / sr);
    m.r = float(exp(-kLn1000 / (std::max(0.005, t60) * sr)));
    m.re = float(mode.gain * weight);  // start at phase 0 of a sine: output (im) starts at 0
    m.im = 0;
  }

  voice.sweep_amount = sound.sweep;
  voice.sweep_env = 1.0f;
  voice.sweep_coef16 = sound.sweep != 0 ? float(exp(-16.0 / (std::max(1.0f, sound.sweep_ms) * 0.001 * sr))) : 0.0f;
  voice.block_counter = 16;
  UpdateRotation(voice, 1.0f + voice.sweep_amount);

  // Noise layer through a band-pass (RBJ, 0 dB peak).
  voice.noise_env = 0;
  if (sound.noise > 0 && sound.noise_hz > 0) {
    double w0 = 2.0 * kPi * std::min(double(sound.noise_hz), 0.45 * sr) / sr;
    double alpha = sin(w0) / (2.0 * std::max(0.1f, sound.noise_q));
    double a0 = 1.0 + alpha;
    voice.b0 = float(alpha / a0);
    voice.b2 = float(-alpha / a0);
    voice.a1 = float(-2.0 * cos(w0) / a0);
    voice.a2 = float((1.0 - alpha) / a0);
    voice.z1 = voice.z2 = 0;
    voice.noise_env = float(sound.noise * (0.4 + 0.6 * velocity));
    voice.noise_coef = float(exp(-kLn1000 / (std::max(0.005f, sound.noise_t60) * sr)));
  }
  voice.click_env = float(sound.click * velocity);
  voice.click_coef = float(exp(-1.0 / (0.0012 * sr)));
  voice.attack = sound.attack_ms > 0 ? 1.0f : 0.0f;
  voice.attack_coef = sound.attack_ms > 0 ? float(exp(-1.0 / (sound.attack_ms * 0.001 * sr))) : 0.0f;
  voice.trem_phase = 0;
  voice.trem_inc = pitched ? float(2.0 * kPi * params_.tremolo_hz / sr) : 0.0f;
  voice.trem_depth = pitched ? params_.tremolo_depth : 0.0f;
  // Drum hits are balanced against each other in the table; the whole kit sits ~6 dB down
  // so a full mix leaves headroom.
  voice.gain = sound.gain * (pitched ? 1.0f : 0.5f);
  voice.choke_group = sound.choke_group;
  voice.level = 1.0f;
  voice.active = true;
}

void ModalInstrument::Damp(Voice& voice, real_t t60) {
  const float r_new = float(exp(-kLn1000 / (std::max(0.005, double(t60)) * sample_rate_)));
  for (int k = 0; k < voice.mode_count; ++k) {
    VoiceMode& m = voice.modes[k];
    if (r_new < m.r) {
      float ratio = r_new / m.r;
      m.c *= ratio; m.s *= ratio; m.r = r_new;
    }
  }
  voice.noise_coef = std::min(voice.noise_coef, r_new);
}

int ModalInstrument::NoteOn(int v) {
  const auto& nd = cdata_->note_data_array[v];
  Voice& voice = voices_[v];
  if (kit_) {
    if (nd.note < kKitFirstNote || nd.note > kKitLastNote) {
      voice.active = false;
      return S_OK;
    }
    const ModalSound& sound = kit_sounds_[nd.note - kKitFirstNote];
    if (sound.mode_count == 0 && sound.noise == 0 && sound.click == 0) {
      voice.active = false;  // unmapped
      return S_OK;
    }
    // Choke: e.g. a closed hi-hat stops a ringing open one.
    if (sound.choke_group != 0) {
      for (int u = 0; u < Polyphony; ++u)
        if (u != v && voices_[u].active && voices_[u].choke_group == sound.choke_group)
          Damp(voices_[u], 0.04f);
    }
    Start(voice, sound, 0, nd.velocity, false);
  } else {
    Start(voice, params_.sound, voice.freq, nd.velocity, true);
  }
  return S_OK;
}

int ModalInstrument::NoteOff(int v) {
  // Drums and undamped mallets ring on; damped instruments (vibraphone, celesta) stop.
  if (!kit_ && params_.release_t60_s > 0 && voices_[v].active)
    Damp(voices_[v], params_.release_t60_s);
  return S_OK;
}

void ModalInstrument::BendFrequency(real_t freq, int v) {
  Voice& voice = voices_[v];
  if (kit_ || !voice.active || voice.freq <= 0)
    return;
  const float ratio = float(freq / voice.freq);
  voice.freq = freq;
  for (int k = 0; k < voice.mode_count; ++k)
    voice.modes[k].w *= ratio;
  UpdateRotation(voice, 1.0f + voice.sweep_amount * voice.sweep_env);
}

real_t ModalInstrument::Tick(int v) {
  Voice& voice = voices_[v];
  if (!voice.active)
    return 0;

  if (voice.sweep_amount != 0 && --voice.block_counter <= 0) {
    voice.block_counter = 16;
    voice.sweep_env *= voice.sweep_coef16;
    UpdateRotation(voice, 1.0f + voice.sweep_amount * voice.sweep_env);
  }

  float out = 0;
  for (int k = 0; k < voice.mode_count; ++k) {
    VoiceMode& m = voice.modes[k];
    float re = m.re * m.c - m.im * m.s;
    float im = m.re * m.s + m.im * m.c;
    m.re = re; m.im = im;
    out += im;
  }
  if (voice.noise_env > 1e-6f) {
    float x = Random();
    float y = voice.b0 * x + voice.z1;
    voice.z1 = -voice.a1 * y + voice.z2;
    voice.z2 = voice.b2 * x - voice.a2 * y;
    out += y * voice.noise_env;
    voice.noise_env *= voice.noise_coef;
  }
  if (voice.click_env > 1e-6f) {
    out += Random() * voice.click_env;
    voice.click_env *= voice.click_coef;
  }
  if (voice.attack > 1e-4f) {
    out *= 1.0f - voice.attack;
    voice.attack *= voice.attack_coef;
  }
  if (voice.trem_depth > 0) {
    out *= 1.0f - voice.trem_depth * 0.5f * (1.0f + sinf(voice.trem_phase));
    voice.trem_phase += voice.trem_inc;
    if (voice.trem_phase > float(2 * kPi)) voice.trem_phase -= float(2 * kPi);
  }

  out *= voice.gain;
  float mag = fabsf(out);
  voice.level = mag > voice.level ? mag : voice.level * level_decay_;
  if (voice.level < 1e-5f)
    voice.active = false;  // below -100 dB
  return out;
}

// ---- Pitched presets: mode ratios, relative gains and relative decay -------------------------

namespace {
ModalParams Pitched(const ModalSound& sound, real_t t60_c4, real_t slope, real_t release = 0) {
  ModalParams p = {};
  p.sound = sound;
  p.t60_c4_s = t60_c4;
  p.t60_slope = slope;
  p.release_t60_s = release;
  return p;
}
}  // namespace

ModalParams ModalInstrument::Marimba() {  // tuned bar 1 : 4 : 10, rosewood, resonators
  return Pitched(Noise(Mallet(Sound({{1.0f, 1.0f, 1.0f}, {3.93f, 0.35f, 0.35f}, {9.42f, 0.12f, 0.15f}}, 0.8f),
                              3000.0f, 1.5f), 0.03f, 800.0f, 0.7f, 0.03f), 1.2f, 0.8f);
}
ModalParams ModalInstrument::Xylophone() {  // tuned 1 : 3, hard mallets, short
  return Pitched(Noise(Mallet(Sound({{1.0f, 1.0f, 1.0f}, {3.0f, 0.5f, 0.5f}, {6.0f, 0.2f, 0.3f}, {9.9f, 0.08f, 0.2f}}, 0.7f),
                              6000.0f, 0.5f), 0.05f, 2000.0f, 0.7f, 0.02f), 0.6f, 0.6f);
}
ModalParams ModalInstrument::Vibraphone() {  // metal bars 1 : 4 : 10, long ring, motor tremolo, dampers
  ModalParams p = Pitched(Mallet(Sound({{1.0f, 1.0f, 1.0f}, {4.0f, 0.25f, 0.3f}, {10.0f, 0.08f, 0.1f}}, 0.7f), 2500.0f, 2.0f),
                          4.0f, 0.5f, 0.35f);
  p.tremolo_hz = 5.5f; p.tremolo_depth = 0.25f;
  return p;
}
ModalParams ModalInstrument::Glockenspiel() {  // free steel bars, hard mallets
  return Pitched(Mallet(Bar(1.0f, 1.0f, 0.55f), 9000.0f, 0.3f), 6.0f, 0.4f);
}
ModalParams ModalInstrument::Celesta() {  // struck plates over resonators, felt hammers, dampers
  return Pitched(Mallet(Sound({{1.0f, 1.0f, 1.0f}, {4.0f, 0.15f, 0.4f}, {10.1f, 0.05f, 0.2f}}, 0.7f), 3000.0f, 1.0f),
                 1.8f, 0.5f, 0.3f);
}
ModalParams ModalInstrument::MusicBox() {  // plucked steel tines
  return Pitched(Mallet(Sound({{1.0f, 1.0f, 1.0f}, {5.9f, 0.3f, 0.3f}, {12.1f, 0.1f, 0.15f}}, 0.6f), 8000.0f, 0.2f), 1.5f, 0.5f);
}
ModalParams ModalInstrument::TubularBells() {  // bell partials: hum, prime, tierce, quint, nominal...
  return Pitched(Mallet(Sound({{0.5f, 0.4f, 1.5f}, {1.0f, 1.0f, 1.0f}, {1.19f, 0.6f, 0.8f}, {1.5f, 0.4f, 0.7f},
                               {2.0f, 0.5f, 0.6f}, {2.5f, 0.25f, 0.45f}, {3.0f, 0.15f, 0.35f}}, 0.4f), 6000.0f, 0.5f),
                 6.0f, 0.3f);
}
ModalParams ModalInstrument::SteelDrums() {  // hand-tuned harmonic notes on a steel pan
  return Pitched(Noise(Mallet(Sound({{1.0f, 1.0f, 1.0f}, {2.0f, 0.5f, 0.7f}, {3.0f, 0.3f, 0.5f}, {4.0f, 0.1f, 0.4f}}, 0.6f),
                              4000.0f, 1.0f), 0.02f, 1500.0f, 0.7f, 0.03f), 1.2f, 0.5f);
}
ModalParams ModalInstrument::Kalimba() {
  return Pitched(Mallet(Sound({{1.0f, 1.0f, 1.0f}, {5.9f, 0.25f, 0.3f}, {11.2f, 0.08f, 0.15f}}, 0.7f), 4000.0f, 0.5f), 1.5f, 0.5f);
}
ModalParams ModalInstrument::Timpani() {  // air-loaded membrane: nearly harmonic 1 : 1.5 : 2 : 2.44 : 2.9
  return Pitched(Noise(Mallet(Sound({{1.0f, 1.0f, 1.0f}, {1.5f, 0.6f, 0.8f}, {1.98f, 0.4f, 0.6f}, {2.44f, 0.25f, 0.5f},
                                     {2.9f, 0.15f, 0.4f}}, 0.6f), 1500.0f, 2.0f), 0.05f, 300.0f, 0.7f, 0.05f),
                 2.5f, 0.3f);
}
ModalParams ModalInstrument::Agogo() {
  return Pitched(Sound({{1.0f, 1.0f, 1.0f}, {2.53f, 0.5f, 0.6f}, {4.6f, 0.2f, 0.4f}}, 0.5f), 0.6f, 0.2f);
}
ModalParams ModalInstrument::Woodblock() {
  return Pitched(Click(Noise(Sound({{1.0f, 1.0f, 1.0f}, {2.6f, 0.4f, 0.6f}, {4.8f, 0.2f, 0.4f}}, 0.6f), 0.1f, 2000.0f, 0.7f, 0.01f),
                       0.2f), 0.12f, 0.0f);
}
ModalParams ModalInstrument::Taiko() {
  return Pitched(Click(Sweep(Membrane(1.0f, 1.0f, 1.0f, 5), 0.2f, 30.0f), 0.2f), 0.8f, 0.3f);
}
ModalParams ModalInstrument::SynthDrum() {  // classic analog tom: one sine with a pitch drop
  return Pitched(Sweep(Sound({{1.0f, 1.0f, 1.0f}}, 0.9f), 1.0f, 60.0f), 0.5f, 0.3f);
}

}
}
}
}
