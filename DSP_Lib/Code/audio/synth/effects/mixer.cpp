#include <math.h>
#include <stdio.h>
#include <string.h>
#include <xmmintrin.h>
#include <algorithm>
#include <string>
#include <vector>
#include "mixer.h"
#include "biquad.h"

namespace dsp {
namespace audio {
namespace synth {
namespace effects {

// ---------------------------------------------------------------------------------------------
// Parameter tables
// ---------------------------------------------------------------------------------------------
namespace {

using U = ParamUnit;

const ParamInfo kEqInfo[kEqParamCount] = {
  {"EQ", 0, 1, 0, U::kSwitch, false},
  {"Low", 20, 1000, 100, U::kHz, true},     {"Gain", -18, 18, 0, U::kDb, false}, {"Q", 0.3f, 2.0f, 0.707f, U::kQ, true},
  {"Lo-Mid", 50, 5000, 400, U::kHz, true},  {"Gain", -18, 18, 0, U::kDb, false}, {"Q", 0.2f, 10.0f, 1.0f, U::kQ, true},
  {"Hi-Mid", 300, 15000, 2500, U::kHz, true}, {"Gain", -18, 18, 0, U::kDb, false}, {"Q", 0.2f, 10.0f, 1.0f, U::kQ, true},
  {"High", 1500, 20000, 8000, U::kHz, true}, {"Gain", -18, 18, 0, U::kDb, false}, {"Q", 0.3f, 2.0f, 0.707f, U::kQ, true},
};

const ParamInfo kStripCompInfo[kCompParamCount] = {
  {"Comp", 0, 1, 0, U::kSwitch, false},
  {"Thresh", -60, 0, -18, U::kDb, false},
  {"Ratio", 1, 20, 4, U::kRatio, true},
  {"Attack", 0.1f, 100, 10, U::kMs, true},
  {"Release", 10, 2000, 150, U::kMs, true},
  {"Knee", 0, 18, 6, U::kDb, false},
  {"Makeup", 0, 24, 0, U::kDb, false},
};

const ParamInfo kMasterCompInfo[kCompParamCount] = {
  {"Comp", 0, 1, 0, U::kSwitch, false},
  {"Thresh", -40, 0, -12, U::kDb, false},
  {"Ratio", 1, 10, 2, U::kRatio, true},
  {"Attack", 0.1f, 100, 20, U::kMs, true},
  {"Release", 10, 2000, 250, U::kMs, true},
  {"Knee", 0, 18, 6, U::kDb, false},
  {"Makeup", 0, 18, 0, U::kDb, false},
};

struct StripTable {
  ParamInfo info[kStripParamCount];
  StripTable() {
    info[kStripTrim] = {"Trim", -24, 24, 0, U::kDb, false};
    info[kStripPhase] = {"Phase", 0, 1, 0, U::kSwitch, false};
    info[kStripHpfOn] = {"HPF", 0, 1, 0, U::kSwitch, false};
    info[kStripHpfFreq] = {"Low cut", 20, 2000, 80, U::kHz, true};
    info[kStripHpfSteep] = {"24 dB", 0, 1, 0, U::kSwitch, false};
    info[kStripLpfOn] = {"LPF", 0, 1, 0, U::kSwitch, false};
    info[kStripLpfFreq] = {"High cut", 500, 20000, 12000, U::kHz, true};
    info[kStripLpfQ] = {"Reso", 0.5f, 8.0f, 0.707f, U::kQ, true};
    info[kStripDriveOn] = {"Drive", 0, 1, 0, U::kSwitch, false};
    info[kStripDrive] = {"Drive", 0, 36, 12, U::kDb, false};
    info[kStripDriveMix] = {"Mix", 0, 1, 1, U::kPercent, false};
    for (int i = 0; i < kEqParamCount; ++i) info[kStripEq + i] = kEqInfo[i];
    for (int i = 0; i < kCompParamCount; ++i) info[kStripComp + i] = kStripCompInfo[i];
    info[kStripDynFirst] = {"Dyn first", 0, 1, 0, U::kSwitch, false};
    info[kStripVolume] = {"Volume", -60, 12, 0, U::kFader, false};
    info[kStripPan] = {"Pan", -1, 1, 0, U::kPan, false};
    info[kStripMute] = {"Mute", 0, 1, 0, U::kSwitch, false};
    info[kStripSolo] = {"Solo", 0, 1, 0, U::kSwitch, false};
    info[kStripSendPre] = {"Pre-fader", 0, 1, 0, U::kSwitch, false};
    info[kStripSendReverb] = {"Reverb", 0, 1, 0, U::kPercent, false};
    info[kStripSendDelay] = {"Delay", 0, 1, 0, U::kPercent, false};
    info[kStripSendChorus] = {"Chorus", 0, 1, 0, U::kPercent, false};
  }
};

struct MasterTable {
  ParamInfo info[kMasterParamCount];
  MasterTable() {
    info[kRevSize] = {"Size", 0, 1, 0.75f, U::kPercent, false};
    info[kRevDamp] = {"Damping", 0, 1, 0.5f, U::kPercent, false};
    info[kRevWidth] = {"Width", 0, 1, 1, U::kPercent, false};
    info[kRevPreDelay] = {"Pre-delay", 0, 200, 12, U::kMs, false};
    info[kRevReturn] = {"Return", 0, 1, 0.7f, U::kPercent, false};
    info[kDlyTime] = {"Time", 20, 2000, 375, U::kMs, true};
    info[kDlyFeedback] = {"Feedback", 0, 0.95f, 0.35f, U::kPercent, false};
    info[kDlyDamp] = {"Tone", 500, 20000, 6000, U::kHz, true};
    info[kDlyPingPong] = {"Ping-pong", 0, 1, 1, U::kSwitch, false};
    info[kDlyReturn] = {"Return", 0, 1, 0.6f, U::kPercent, false};
    info[kChoRate] = {"Rate", 0.05f, 5, 0.6f, U::kHz, true};
    info[kChoDepth] = {"Depth", 0, 1, 0.5f, U::kPercent, false};
    info[kChoReturn] = {"Return", 0, 1, 0.8f, U::kPercent, false};
    for (int i = 0; i < kEqParamCount; ++i) info[kMasterEq + i] = kEqInfo[i];
    for (int i = 0; i < kCompParamCount; ++i) info[kMasterComp + i] = kMasterCompInfo[i];
    info[kMasterWidth] = {"Width", 0, 2, 1, U::kTimes, false};
    info[kMasterGain] = {"Output", -24, 12, 0, U::kDb, false};
  }
};

const StripTable& strip_table() { static StripTable t; return t; }
const MasterTable& master_table() { static MasterTable t; return t; }

// Fader travel: dB at control positions 0, .2, .45, .75 and 1 (0 dB at three quarters).
const float kFaderPos[] = {0.0f, 0.2f, 0.45f, 0.75f, 1.0f};
const float kFaderDb[] = {-60.0f, -36.0f, -12.0f, 0.0f, 12.0f};

inline float DbToGain(float db) { return expf(db * 0.11512925465f); }  // ln(10)/20
inline float Lerp(float a, float b, float t) { return a + (b - a) * t; }

}  // namespace

const ParamInfo& StripParamInfo(int id) { return strip_table().info[id]; }
const ParamInfo& MasterParamInfo(int id) { return master_table().info[id]; }

namespace {
const char* const kEqKeys[kEqParamCount] = {
  "on", "low.freq", "low.gain", "low.q", "lomid.freq", "lomid.gain", "lomid.q",
  "himid.freq", "himid.gain", "himid.q", "high.freq", "high.gain", "high.q"};
const char* const kCompKeys[kCompParamCount] = {"on", "threshold", "ratio", "attack", "release", "knee", "makeup"};

struct KeyTables {
  std::string strip[kStripParamCount], master[kMasterParamCount];
  KeyTables() {
    const char* s[kStripParamCount] = {};
    s[kStripTrim] = "trim"; s[kStripPhase] = "phase";
    s[kStripHpfOn] = "hpf.on"; s[kStripHpfFreq] = "hpf.freq"; s[kStripHpfSteep] = "hpf.steep";
    s[kStripLpfOn] = "lpf.on"; s[kStripLpfFreq] = "lpf.freq"; s[kStripLpfQ] = "lpf.q";
    s[kStripDriveOn] = "drive.on"; s[kStripDrive] = "drive.amount"; s[kStripDriveMix] = "drive.mix";
    s[kStripDynFirst] = "comp.before_eq";
    s[kStripVolume] = "volume"; s[kStripPan] = "pan"; s[kStripMute] = "mute"; s[kStripSolo] = "solo";
    s[kStripSendPre] = "send.prefader"; s[kStripSendReverb] = "send.reverb";
    s[kStripSendDelay] = "send.delay"; s[kStripSendChorus] = "send.chorus";
    for (int i = 0; i < kStripParamCount; ++i) if (s[i]) strip[i] = s[i];
    for (int i = 0; i < kEqParamCount; ++i) strip[kStripEq + i] = std::string("eq.") + kEqKeys[i];
    for (int i = 0; i < kCompParamCount; ++i) strip[kStripComp + i] = std::string("comp.") + kCompKeys[i];

    const char* m[kMasterParamCount] = {};
    m[kRevSize] = "reverb.size"; m[kRevDamp] = "reverb.damping"; m[kRevWidth] = "reverb.width";
    m[kRevPreDelay] = "reverb.predelay"; m[kRevReturn] = "reverb.return";
    m[kDlyTime] = "delay.time"; m[kDlyFeedback] = "delay.feedback"; m[kDlyDamp] = "delay.tone";
    m[kDlyPingPong] = "delay.pingpong"; m[kDlyReturn] = "delay.return";
    m[kChoRate] = "chorus.rate"; m[kChoDepth] = "chorus.depth"; m[kChoReturn] = "chorus.return";
    m[kMasterWidth] = "width"; m[kMasterGain] = "output";
    for (int i = 0; i < kMasterParamCount; ++i) if (m[i]) master[i] = m[i];
    for (int i = 0; i < kEqParamCount; ++i) master[kMasterEq + i] = std::string("eq.") + kEqKeys[i];
    for (int i = 0; i < kCompParamCount; ++i) master[kMasterComp + i] = std::string("comp.") + kCompKeys[i];
  }
};
const KeyTables& key_tables() { static KeyTables t; return t; }
}  // namespace

const char* StripParamKey(int id) { return key_tables().strip[id].c_str(); }
const char* MasterParamKey(int id) { return key_tables().master[id].c_str(); }
int FindStripParam(const char* key) {
  for (int i = 0; i < kStripParamCount; ++i) if (key_tables().strip[i] == key) return i;
  return -1;
}
int FindMasterParam(const char* key) {
  for (int i = 0; i < kMasterParamCount; ++i) if (key_tables().master[i] == key) return i;
  return -1;
}

float ParamToNormalized(const ParamInfo& info, float v) {
  v = std::clamp(v, info.min, info.max);
  if (info.unit == ParamUnit::kFader) {
    for (int i = 1; i < 5; ++i)
      if (v <= kFaderDb[i])
        return Lerp(kFaderPos[i - 1], kFaderPos[i], (v - kFaderDb[i - 1]) / (kFaderDb[i] - kFaderDb[i - 1]));
    return 1.0f;
  }
  if (info.log && info.min > 0)
    return logf(v / info.min) / logf(info.max / info.min);
  return (v - info.min) / (info.max - info.min);
}

float ParamFromNormalized(const ParamInfo& info, float t) {
  t = std::clamp(t, 0.0f, 1.0f);
  if (info.unit == ParamUnit::kFader) {
    for (int i = 1; i < 5; ++i)
      if (t <= kFaderPos[i])
        return Lerp(kFaderDb[i - 1], kFaderDb[i], (t - kFaderPos[i - 1]) / (kFaderPos[i] - kFaderPos[i - 1]));
    return kFaderDb[4];
  }
  if (info.unit == ParamUnit::kSwitch)
    return t >= 0.5f ? info.max : info.min;
  if (info.log && info.min > 0)
    return info.min * powf(info.max / info.min, t);
  return info.min + t * (info.max - info.min);
}

void FormatParam(const ParamInfo& info, float v, char* out, size_t n) {
  switch (info.unit) {
    case ParamUnit::kFader:
      if (v <= info.min + 0.05f) { snprintf(out, n, "-inf"); return; }
      snprintf(out, n, "%+.1f dB", v);
      return;
    case ParamUnit::kDb:
      snprintf(out, n, fabsf(v) < 0.05f ? "0.0 dB" : "%+.1f dB", v);
      return;
    case ParamUnit::kHz:
      if (v < 10) snprintf(out, n, "%.2f Hz", v);
      else if (v < 1000) snprintf(out, n, "%.0f Hz", v);
      else snprintf(out, n, "%.1f kHz", v / 1000.0f);
      return;
    case ParamUnit::kMs:
      if (v < 10) snprintf(out, n, "%.1f ms", v);
      else if (v < 1000) snprintf(out, n, "%.0f ms", v);
      else snprintf(out, n, "%.2f s", v / 1000.0f);
      return;
    case ParamUnit::kPercent: snprintf(out, n, "%.0f%%", v * 100.0f); return;
    case ParamUnit::kRatio: snprintf(out, n, v >= 19.95f ? "inf:1" : "%.1f:1", v); return;
    case ParamUnit::kQ: snprintf(out, n, "Q %.2f", v); return;
    case ParamUnit::kTimes: snprintf(out, n, "%.0f%%", v * 100.0f); return;
    case ParamUnit::kPan: {
      int p = int(lroundf(v * 100.0f));
      if (p == 0) snprintf(out, n, "C");
      else snprintf(out, n, "%c%d", p < 0 ? 'L' : 'R', p < 0 ? -p : p);
      return;
    }
    case ParamUnit::kSwitch: snprintf(out, n, v >= 0.5f ? "On" : "Off"); return;
    default: snprintf(out, n, "%.2f", v); return;
  }
}

static BiquadType EqBandType(int band) {
  return band == 0 ? BiquadType::kLowShelf : band == kEqBands - 1 ? BiquadType::kHighShelf : BiquadType::kPeak;
}

double EqResponseDb(const float* eq, double fs, double freq) {
  double db = 0;
  for (int b = 0; b < kEqBands; ++b) {
    float gain = eq[EqBandGain(b)];
    if (fabsf(gain) < 0.01f) continue;
    auto c = DesignBiquad(EqBandType(b), fs, eq[EqBandFreq(b)], eq[EqBandQ(b)], gain);
    db += BiquadMagnitudeDb(c, fs, freq);
  }
  return db;
}

// ---------------------------------------------------------------------------------------------
// Building blocks (audio thread only)
// ---------------------------------------------------------------------------------------------
namespace {

// Log-domain peak compressor with soft knee; stereo-linked by the caller.
struct Compressor {
  float threshold = -18, ratio = 4, knee = 6, makeup = 0;
  float attack = 0, release = 0;
  float env_db = 0;  // current gain reduction (<= 0 dB)

  void Configure(float fs, const float* p) {  // p = compressor parameter block
    threshold = p[kCompThreshold];
    ratio = std::max(1.0f, p[kCompRatio]);
    knee = std::max(0.0f, p[kCompKnee]);
    makeup = p[kCompMakeup];
    attack = expf(-1.0f / (std::max(0.05f, p[kCompAttack]) * 0.001f * fs));
    release = expf(-1.0f / (std::max(1.0f, p[kCompRelease]) * 0.001f * fs));
  }
  // Returns the gain (linear, including makeup) for a detector level.
  inline float Gain(float level) {
    const float x = 20.0f * log10f(level + 1e-9f);
    const float over = x - threshold;
    const float slope = 1.0f / ratio - 1.0f;
    float gr;
    if (2.0f * over <= -knee) gr = 0.0f;
    else if (knee > 0.0f && 2.0f * fabsf(over) < knee) {
      const float k = over + knee * 0.5f;
      gr = slope * k * k / (2.0f * knee);
    } else gr = slope * over;
    const float coef = gr < env_db ? attack : release;
    env_db = gr + coef * (env_db - gr);
    return DbToGain(env_db + makeup);
  }
  float reduction_gain() const { return DbToGain(env_db); }
};

// Soft clipper, level-matched around -12 dBFS.
inline float Saturate(float x, float pre, float norm) { return tanhf(x * pre) * norm; }

// Freeverb (Jezar at Dreampoint, public domain): 8 damped combs + 4 allpasses per side.
struct Comb {
  std::vector<float> buf;
  int idx = 0;
  float store = 0;
  inline float Tick(float in, float fb, float d1, float d2) {
    const float out = buf[idx];
    store = out * d2 + store * d1;
    buf[idx] = in + store * fb;
    if (++idx >= int(buf.size())) idx = 0;
    return out;
  }
  void Clear() { std::fill(buf.begin(), buf.end(), 0.0f); store = 0; }
};

struct Allpass {
  std::vector<float> buf;
  int idx = 0;
  inline float Tick(float in) {
    const float b = buf[idx];
    buf[idx] = in + b * 0.5f;
    if (++idx >= int(buf.size())) idx = 0;
    return b - in;
  }
  void Clear() { std::fill(buf.begin(), buf.end(), 0.0f); }
};

struct Reverb {
  Comb comb[2][8];
  Allpass ap[2][4];
  std::vector<float> pre[2];
  int pre_idx = 0;

  void Init(uint32_t fs) {
    static const int kComb[8] = {1116, 1188, 1277, 1356, 1422, 1491, 1557, 1617};
    static const int kAp[4] = {556, 441, 341, 225};
    const double scale = fs / 44100.0;
    for (int s = 0; s < 2; ++s) {
      const int spread = s ? 23 : 0;
      for (int i = 0; i < 8; ++i) comb[s][i].buf.assign(size_t((kComb[i] + spread) * scale), 0.0f);
      for (int i = 0; i < 4; ++i) ap[s][i].buf.assign(size_t((kAp[i] + spread) * scale), 0.0f);
      pre[s].assign(size_t(fs * 0.25) + 1, 0.0f);
    }
  }
  // Returns the wet signal (scaled for wet = 1 at return 1).
  inline void Tick(float in_l, float in_r, int pre_samples, float fb, float damp, float width, float& out_l, float& out_r) {
    const int n = int(pre[0].size());
    pre[0][pre_idx] = in_l;
    pre[1][pre_idx] = in_r;
    int rd = pre_idx - pre_samples;
    if (rd < 0) rd += n;
    if (++pre_idx >= n) pre_idx = 0;
    const float input = (pre[0][rd] + pre[1][rd]) * 0.015f;
    const float d1 = damp, d2 = 1.0f - damp;
    float o[2] = {0, 0};
    for (int s = 0; s < 2; ++s) {
      float acc = 0;
      for (int i = 0; i < 8; ++i) acc += comb[s][i].Tick(input, fb, d1, d2);
      for (int i = 0; i < 4; ++i) acc = ap[s][i].Tick(acc);
      o[s] = acc;
    }
    const float wet1 = 3.0f * (width * 0.5f + 0.5f), wet2 = 3.0f * ((1.0f - width) * 0.5f);
    out_l = o[0] * wet1 + o[1] * wet2;
    out_r = o[1] * wet1 + o[0] * wet2;
  }
};

// Fractional delay line.
struct DelayLine {
  std::vector<float> buf;
  int w = 0;
  void Init(size_t n) { buf.assign(n, 0.0f); w = 0; }
  inline void Write(float x) {
    buf[w] = x;
    if (++w >= int(buf.size())) w = 0;
  }
  // Delay in samples (>= 1) behind the most recent write.
  inline float Read(float d) const {
    // Double precision: in float the wrapped position (~1e5) only resolves ~0.008 samples
    // and can round up to exactly n.
    const int n = int(buf.size());
    double pos = double(w) - 1.0 - double(d);
    while (pos < 0) pos += n;
    int i0 = int(pos);
    const float frac = float(pos - i0);
    while (i0 >= n) i0 -= n;
    int i1 = i0 + 1;
    if (i1 >= n) i1 -= n;
    return buf[i0] + (buf[i1] - buf[i0]) * frac;
  }
};

struct StereoDelay {
  DelayLine line[2];
  float lp[2] = {0, 0};
  float cur_delay = -1;
  void Init(uint32_t fs) {
    line[0].Init(size_t(fs * 2.1));
    line[1].Init(size_t(fs * 2.1));
  }
  inline void Tick(float in_l, float in_r, float delay, float fb, float damp_coef, bool ping_pong, float& out_l, float& out_r) {
    if (cur_delay < 0) cur_delay = delay;
    cur_delay += (delay - cur_delay) * 0.0005f;  // glide instead of jumping (no clicks)
    const float dl = line[0].Read(cur_delay), dr = line[1].Read(cur_delay);
    lp[0] += (dl - lp[0]) * damp_coef;
    lp[1] += (dr - lp[1]) * damp_coef;
    if (ping_pong) {
      line[0].Write((in_l + in_r) * 0.5f + lp[1] * fb);
      line[1].Write(lp[0] * fb);
    } else {
      line[0].Write(in_l + lp[0] * fb);
      line[1].Write(in_r + lp[1] * fb);
    }
    out_l = dl;
    out_r = dr;
  }
};

struct Chorus {
  DelayLine line[2];
  double phase = 0;
  void Init(uint32_t fs) {
    line[0].Init(size_t(fs * 0.05));
    line[1].Init(size_t(fs * 0.05));
  }
  inline void Tick(float in_l, float in_r, float fs, float rate, float depth, float& out_l, float& out_r) {
    const double kTwoPi = 6.283185307179586;
    phase += rate / fs;
    if (phase >= 1.0) phase -= 1.0;
    const float base = 0.008f * fs, swing = 0.006f * fs * depth;
    const float ml = float(sin(kTwoPi * phase)), mr = float(sin(kTwoPi * (phase + 0.25)));
    line[0].Write(in_l);
    line[1].Write(in_r);
    out_l = line[0].Read(base + swing * ml);
    out_r = line[1].Read(base + swing * mr);
  }
};

// One-pole smoothing of a whole parameter bank, once per sub-block.
template <int N, typename Bank>
void SmoothParams(float (&sm)[N], const Bank& bank, float k, bool snap) {
  for (int i = 0; i < N; ++i) {
    const float target = bank.get(i);
    if (snap) { sm[i] = target; continue; }
    const float d = target - sm[i];
    if (fabsf(d) <= 1e-4f * std::max(1.0f, fabsf(target))) sm[i] = target;
    else sm[i] += d * k;
  }
}

// 4-band EQ state with coefficient cache.
struct EqState {
  StereoBiquad band[kEqBands];
  float cache[kEqParamCount];
  bool active[kEqBands] = {false, false, false, false};
  EqState() { for (auto& c : cache) c = -1e9f; }
  void Update(const float* eq, double fs) {
    for (int b = 0; b < kEqBands; ++b) {
      const float f = eq[EqBandFreq(b)], g = eq[EqBandGain(b)], q = eq[EqBandQ(b)];
      const bool on = fabsf(g) >= 0.01f;
      if (!on) {
        if (active[b]) band[b].Reset();
        active[b] = false;
        continue;
      }
      active[b] = true;
      if (f != cache[EqBandFreq(b)] || g != cache[EqBandGain(b)] || q != cache[EqBandQ(b)]) {
        band[b].c = DesignBiquad(EqBandType(b), fs, f, q, g);
        cache[EqBandFreq(b)] = f;
        cache[EqBandGain(b)] = g;
        cache[EqBandQ(b)] = q;
      }
    }
  }
  inline void Process(float& l, float& r) {
    for (int b = 0; b < kEqBands; ++b)
      if (active[b]) band[b].Process(l, r);
  }
  void Reset() {
    for (auto& b : band) b.Reset();
  }
};

}  // namespace

// ---------------------------------------------------------------------------------------------
// Strip and master state
// ---------------------------------------------------------------------------------------------
struct Mixer::StripState {
  float sm[kStripParamCount];
  bool primed = false;
  // Per-sub-block derived values; the previous block's values are ramped to the new ones.
  struct Derived {
    float trim = 1, hpf = 0, hpf2 = 0, drive = 0, lpf = 0, eq = 0, comp = 0;
    float mute = 1, fader_l = 1, fader_r = 1, send_pre = 0, send[3] = {0, 0, 0};
  } d;
  StereoBiquad hpf[2], lpf;
  float cache_hpf = -1, cache_lpf = -1, cache_lpf_q = -1;
  EqState eq;
  Compressor comp;
  float cache_comp[kCompParamCount];
  float drive_pre = 1, drive_norm = 1, cache_drive = -1e9f;
  float gate = 1;  // mute/solo
  uint32_t silent = 0;  // frames of silent input
  StripState() { for (auto& c : cache_comp) c = -1e9f; }
};

struct Mixer::MasterState {
  float sm[kMasterParamCount];
  bool primed = false;
  struct Derived {
    float ret[3] = {0, 0, 0}, eq = 0, comp = 0, width = 1, gain = 1;
  } d;
  Reverb reverb;
  StereoDelay delay;
  Chorus chorus;
  uint32_t silent[3] = {0, 0, 0};  // frames since the bus last carried signal
  EqState eq;
  Compressor comp;
  float cache_comp[kCompParamCount];
  MasterState() { for (auto& c : cache_comp) c = -1e9f; }
};

void Mixer::Meter::Publish(float l, float r, float gain) {
  float cur = peak[0].load(std::memory_order_relaxed);
  while (l > cur && !peak[0].compare_exchange_weak(cur, l, std::memory_order_relaxed)) {}
  cur = peak[1].load(std::memory_order_relaxed);
  while (r > cur && !peak[1].compare_exchange_weak(cur, r, std::memory_order_relaxed)) {}
  cur = gr.load(std::memory_order_relaxed);
  while (gain < cur && !gr.compare_exchange_weak(cur, gain, std::memory_order_relaxed)) {}
}

Mixer::Mixer() : scope_pos_(0), analyzer_source_(-1) {
  for (auto& s : scope_) s.store(0.0f, std::memory_order_relaxed);
  ResetAll();
}

Mixer::~Mixer() { Deinitialize(); }

void Mixer::Initialize(uint32_t sample_rate, uint32_t max_frames) {
  Deinitialize();
  sample_rate_ = sample_rate;
  max_frames_ = max_frames;
  strips_ = new StripState[kStrips];
  master_ = new MasterState();
  master_->reverb.Init(sample_rate);
  master_->delay.Init(sample_rate);
  master_->chorus.Init(sample_rate);
  // Effects start idle: nothing has been sent to them yet.
  master_->silent[0] = master_->silent[1] = master_->silent[2] = 0xFFFFFFFFu;
  for (auto& b : bus_) b.assign(size_t(max_frames) * 2, 0.0f);
}

void Mixer::Deinitialize() {
  delete[] strips_;
  strips_ = nullptr;
  delete master_;
  master_ = nullptr;
  for (auto& b : bus_) { b.clear(); b.shrink_to_fit(); }
}

void Mixer::ResetStrip(int i) {
  for (int p = 0; p < kStripParamCount; ++p) strip_params_[i].set(p, StripParamInfo(p).def);
}

void Mixer::ResetMaster() {
  for (int p = 0; p < kMasterParamCount; ++p) master_params_.set(p, MasterParamInfo(p).def);
}

void Mixer::ResetAll() {
  for (int i = 0; i < kStrips; ++i) ResetStrip(i);
  ResetMaster();
}

float Mixer::EffectiveSend(int strip, int bus) const {
  static const int kSendParam[3] = {kStripSendReverb, kStripSendDelay, kStripSendChorus};
  float s = strip_params_[strip].get(kSendParam[bus]);
  if (follow_midi_sends()) s += midi_send(strip, bus);
  return s;
}

bool Mixer::AnySolo() const {
  for (int i = 0; i < kStrips; ++i)
    if (strip_params_[i].get(kStripSolo) >= 0.5f) return true;
  return false;
}

void Mixer::ReadScope(float* dest, int count) const {
  if (count > kScopeSize) count = kScopeSize;
  const uint32_t end = scope_pos_.load(std::memory_order_acquire);
  for (int i = 0; i < count; ++i)
    dest[i] = scope_[(end - uint32_t(count) + uint32_t(i)) & (kScopeSize - 1)].load(std::memory_order_relaxed);
}

void Mixer::WriteScope(const float* stereo, uint32_t frames) {
  uint32_t pos = scope_pos_.load(std::memory_order_relaxed);
  for (uint32_t i = 0; i < frames; ++i, ++pos)
    scope_[pos & (kScopeSize - 1)].store((stereo[2 * i] + stereo[2 * i + 1]) * 0.5f, std::memory_order_relaxed);
  scope_pos_.store(pos, std::memory_order_release);
}

void Mixer::Process(float* const* strip_in, uint32_t frames, float* out) {
  memset(out, 0, sizeof(float) * frames * 2);
  if (strips_ == nullptr || frames == 0)
    return;
  // Flush denormals to zero while the filters and reverb decay.
  const unsigned int csr = _mm_getcsr();
  _mm_setcsr(csr | 0x8040);

  uint32_t done = 0;
  while (done < frames) {  // Process() may be handed more than the buses hold
    const uint32_t n = std::min(frames - done, max_frames_);
    for (auto& b : bus_) memset(b.data(), 0, sizeof(float) * n * 2);
    const bool any_solo = AnySolo();
    for (int s = 0; s < kStrips; ++s)
      ProcessStrip(s, strip_in[s] + done * 2, n, out + done * 2, any_solo);
    ProcessMaster(out + done * 2, n);
    done += n;
  }
  _mm_setcsr(csr);
}

void Mixer::ProcessStrip(int index, float* io, uint32_t frames, float* out, bool any_solo) {
  StripState& st = strips_[index];
  const StripParams& p = strip_params_[index];
  const float fs = float(sample_rate_);
  const float k = 1.0f - expf(-float(kSubBlock) / (fs * 0.005f));  // ~5 ms smoothing
  float* bus[3] = {bus_[0].data(), bus_[1].data(), bus_[2].data()};
  float peak_l = 0, peak_r = 0;

  for (uint32_t off = 0; off < frames; off += kSubBlock) {
    const uint32_t n = std::min<uint32_t>(kSubBlock, frames - off);
    SmoothParams(st.sm, p, k, !st.primed);
    const float* sm = st.sm;
    const bool first = !st.primed;
    st.primed = true;

    // Derived values for the end of this sub-block.
    StripState::Derived prev = st.d, next;
    next.trim = DbToGain(sm[kStripTrim]) * (1.0f - 2.0f * sm[kStripPhase]);
    next.hpf = sm[kStripHpfOn];
    next.hpf2 = sm[kStripHpfOn] * sm[kStripHpfSteep];
    next.drive = sm[kStripDriveOn] * sm[kStripDriveMix];
    next.lpf = sm[kStripLpfOn];
    next.eq = sm[kStripEq + kEqOn];
    next.comp = sm[kStripComp + kCompOn];
    // Mute / solo gate, smoothed so switching never clicks.
    const float audible = p.get(kStripMute) < 0.5f && (!any_solo || p.get(kStripSolo) >= 0.5f) ? 1.0f : 0.0f;
    st.gate = first ? audible : st.gate + (audible - st.gate) * k;
    if (fabsf(st.gate - audible) < 1e-4f) st.gate = audible;
    next.mute = st.gate;
    const float fader = sm[kStripVolume] <= -59.95f ? 0.0f : DbToGain(sm[kStripVolume]);
    const float pan = sm[kStripPan];
    next.fader_l = fader * (pan > 0 ? 1.0f - pan : 1.0f);
    next.fader_r = fader * (pan < 0 ? 1.0f + pan : 1.0f);
    next.send_pre = sm[kStripSendPre];
    const bool follow = follow_midi_sends();  // + the file's CC91/CC93 levels
    next.send[0] = sm[kStripSendReverb] + (follow ? midi_send(index, kBusReverb) : 0.0f);
    next.send[1] = sm[kStripSendDelay] + (follow ? midi_send(index, kBusDelay) : 0.0f);
    next.send[2] = sm[kStripSendChorus] + (follow ? midi_send(index, kBusChorus) : 0.0f);
    if (first) prev = next;
    st.d = next;

    // Coefficients.
    if (next.hpf > 0 || prev.hpf > 0) {
      if (sm[kStripHpfFreq] != st.cache_hpf) {
        st.hpf[0].c = st.hpf[1].c = DesignBiquad(BiquadType::kHighPass, fs, sm[kStripHpfFreq], 0.7071, 0);
        st.cache_hpf = sm[kStripHpfFreq];
      }
    } else { st.hpf[0].Reset(); st.hpf[1].Reset(); }
    if (next.hpf2 <= 0 && prev.hpf2 <= 0) st.hpf[1].Reset();
    if (next.lpf > 0 || prev.lpf > 0) {
      if (sm[kStripLpfFreq] != st.cache_lpf || sm[kStripLpfQ] != st.cache_lpf_q) {
        st.lpf.c = DesignBiquad(BiquadType::kLowPass, fs, sm[kStripLpfFreq], sm[kStripLpfQ], 0);
        st.cache_lpf = sm[kStripLpfFreq];
        st.cache_lpf_q = sm[kStripLpfQ];
      }
    } else st.lpf.Reset();
    if (next.drive > 0 || prev.drive > 0) {
      if (sm[kStripDrive] != st.cache_drive) {
        st.drive_pre = DbToGain(sm[kStripDrive]);
        st.drive_norm = 0.25f / tanhf(0.25f * st.drive_pre);
        st.cache_drive = sm[kStripDrive];
      }
    }
    const bool eq_on = next.eq > 0 || prev.eq > 0;
    if (eq_on) st.eq.Update(sm + kStripEq, fs); else st.eq.Reset();
    const bool comp_on = next.comp > 0 || prev.comp > 0;
    if (comp_on) {
      bool changed = false;
      for (int i = 0; i < kCompParamCount; ++i)
        if (sm[kStripComp + i] != st.cache_comp[i]) { st.cache_comp[i] = sm[kStripComp + i]; changed = true; }
      if (changed) st.comp.Configure(fs, sm + kStripComp);
    } else st.comp.env_db = 0;
    const bool dyn_first = sm[kStripDynFirst] >= 0.5f;
    const bool hpf_on = next.hpf > 0 || prev.hpf > 0, hpf2_on = next.hpf2 > 0 || prev.hpf2 > 0;
    const bool lpf_on = next.lpf > 0 || prev.lpf > 0, drive_on = next.drive > 0 || prev.drive > 0;
    float min_gr = 1.0f;

    // Silent input: once the filters have rung out (or with nothing stateful switched on) the
    // strip outputs exact zeros, so skip the per-sample work. Most channels are idle most of
    // the time.
    float level = 0;
    for (uint32_t i = 0; i < n * 2; ++i) level = std::max(level, fabsf(io[2 * off + i]));
    if (level == 0.0f) {
      const bool stateful = hpf_on || lpf_on || drive_on || eq_on || comp_on;
      if (st.silent < 0xFFFFFFFFu - n) st.silent += n;
      if (!stateful || st.silent > uint32_t(fs * 0.5f)) {
        if (stateful) {
          st.hpf[0].Reset(); st.hpf[1].Reset(); st.lpf.Reset(); st.eq.Reset();
          st.comp.env_db = 0;
        }
        continue;
      }
    } else {
      st.silent = 0;
    }

    const float inv = 1.0f / n;
    for (uint32_t i = 0; i < n; ++i) {
      const float t = (i + 1) * inv;
      float* frame = io + 2 * (off + i);
      float l = frame[0], r = frame[1];

      // PRE
      const float trim = Lerp(prev.trim, next.trim, t);
      l *= trim;
      r *= trim;
      if (hpf_on) {
        const float m = Lerp(prev.hpf, next.hpf, t);
        float hl = l, hr = r;
        st.hpf[0].Process(hl, hr);
        if (hpf2_on) {
          const float m2 = Lerp(prev.hpf2, next.hpf2, t) / std::max(m, 1e-6f);
          float sl = hl, sr = hr;
          st.hpf[1].Process(sl, sr);
          hl += (sl - hl) * m2;
          hr += (sr - hr) * m2;
        }
        l += (hl - l) * m;
        r += (hr - r) * m;
      }
      if (drive_on) {
        const float m = Lerp(prev.drive, next.drive, t);
        l += (Saturate(l, st.drive_pre, st.drive_norm) - l) * m;
        r += (Saturate(r, st.drive_pre, st.drive_norm) - r) * m;
      }
      if (lpf_on) {
        const float m = Lerp(prev.lpf, next.lpf, t);
        float fl = l, fr = r;
        st.lpf.Process(fl, fr);
        l += (fl - l) * m;
        r += (fr - r) * m;
      }

      // EQ and dynamics
      for (int stage = 0; stage < 2; ++stage) {
        const bool do_comp = (stage == 0) == dyn_first;
        if (do_comp) {
          if (comp_on) {
            const float m = Lerp(prev.comp, next.comp, t);
            const float g = st.comp.Gain(std::max(fabsf(l), fabsf(r)));
            l += (l * g - l) * m;
            r += (r * g - r) * m;
            min_gr = std::min(min_gr, st.comp.reduction_gain());
          }
        } else if (eq_on) {
          const float m = Lerp(prev.eq, next.eq, t);
          float el = l, er = r;
          st.eq.Process(el, er);
          l += (el - l) * m;
          r += (er - r) * m;
        }
      }

      // POST: mute/solo, fader, balance, sends
      const float mute = Lerp(prev.mute, next.mute, t);
      const float pre_l = l * mute, pre_r = r * mute;
      l = pre_l * Lerp(prev.fader_l, next.fader_l, t);
      r = pre_r * Lerp(prev.fader_r, next.fader_r, t);
      const float sp = Lerp(prev.send_pre, next.send_pre, t);
      const float tap_l = l + (pre_l - l) * sp, tap_r = r + (pre_r - r) * sp;
      for (int b = 0; b < 3; ++b) {
        const float s = Lerp(prev.send[b], next.send[b], t);
        if (s > 0) {
          bus[b][2 * (off + i)] += tap_l * s;
          bus[b][2 * (off + i) + 1] += tap_r * s;
        }
      }
      frame[0] = l;
      frame[1] = r;
      out[2 * (off + i)] += l;
      out[2 * (off + i) + 1] += r;
      peak_l = std::max(peak_l, fabsf(l));
      peak_r = std::max(peak_r, fabsf(r));
    }
    if (min_gr < 1.0f) strips_meter_[index].Publish(0, 0, min_gr);
  }
  strips_meter_[index].Publish(peak_l, peak_r, 1.0f);
  if (analyzer_source() == index)
    WriteScope(io, frames);
}

void Mixer::ProcessMaster(float* out, uint32_t frames) {
  MasterState& st = *master_;
  const MasterParams& p = master_params_;
  const float fs = float(sample_rate_);
  const float k = 1.0f - expf(-float(kSubBlock) / (fs * 0.005f));
  const float* bus[3] = {bus_[0].data(), bus_[1].data(), bus_[2].data()};
  const uint32_t kIdleAfter[3] = {uint32_t(fs * 12), uint32_t(fs * 30), uint32_t(fs * 0.1f)};
  float peak_l = 0, peak_r = 0;

  for (uint32_t off = 0; off < frames; off += kSubBlock) {
    const uint32_t n = std::min<uint32_t>(kSubBlock, frames - off);
    SmoothParams(st.sm, p, k, !st.primed);
    const float* sm = st.sm;
    const bool first = !st.primed;
    st.primed = true;

    MasterState::Derived prev = st.d, next;
    next.ret[0] = sm[kRevReturn];
    next.ret[1] = sm[kDlyReturn];
    next.ret[2] = sm[kChoReturn];
    next.eq = sm[kMasterEq + kEqOn];
    next.comp = sm[kMasterComp + kCompOn];
    next.width = sm[kMasterWidth];
    next.gain = DbToGain(sm[kMasterGain]);
    if (first) prev = next;
    st.d = next;

    // Which effects have input (or are still ringing out)?
    bool run[3];
    for (int b = 0; b < 3; ++b) {
      float level = 0;
      for (uint32_t i = 0; i < n * 2; ++i) level = std::max(level, fabsf(bus[b][2 * off + i]));
      if (level > 1e-9f) st.silent[b] = 0;
      else if (st.silent[b] < 0xFFFFFFFFu - n) st.silent[b] += n;
      run[b] = st.silent[b] < kIdleAfter[b];
    }

    const int pre_delay = int(sm[kRevPreDelay] * 0.001f * fs);
    const float rev_fb = sm[kRevSize] * 0.28f + 0.7f, rev_damp = sm[kRevDamp] * 0.4f, rev_width = sm[kRevWidth];
    const float dly_samples = std::max(1.0f, sm[kDlyTime] * 0.001f * fs);
    const float dly_fb = sm[kDlyFeedback];
    const float dly_damp = 1.0f - expf(-6.2831853f * sm[kDlyDamp] / fs);
    const bool ping_pong = sm[kDlyPingPong] >= 0.5f;

    const bool eq_on = next.eq > 0 || prev.eq > 0;
    if (eq_on) st.eq.Update(sm + kMasterEq, fs); else st.eq.Reset();
    const bool comp_on = next.comp > 0 || prev.comp > 0;
    if (comp_on) {
      bool changed = false;
      for (int i = 0; i < kCompParamCount; ++i)
        if (sm[kMasterComp + i] != st.cache_comp[i]) { st.cache_comp[i] = sm[kMasterComp + i]; changed = true; }
      if (changed) st.comp.Configure(fs, sm + kMasterComp);
    } else st.comp.env_db = 0;
    float min_gr = 1.0f;

    const float inv = 1.0f / n;
    for (uint32_t i = 0; i < n; ++i) {
      const float t = (i + 1) * inv;
      const uint32_t j = 2 * (off + i);
      float l = out[j], r = out[j + 1];
      float wl, wr;
      if (run[0]) {
        st.reverb.Tick(bus[0][j], bus[0][j + 1], pre_delay, rev_fb, rev_damp, rev_width, wl, wr);
        const float g = Lerp(prev.ret[0], next.ret[0], t);
        l += wl * g;
        r += wr * g;
      }
      if (run[1]) {
        st.delay.Tick(bus[1][j], bus[1][j + 1], dly_samples, dly_fb, dly_damp, ping_pong, wl, wr);
        const float g = Lerp(prev.ret[1], next.ret[1], t);
        l += wl * g;
        r += wr * g;
      }
      if (run[2]) {
        st.chorus.Tick(bus[2][j], bus[2][j + 1], fs, sm[kChoRate], sm[kChoDepth], wl, wr);
        const float g = Lerp(prev.ret[2], next.ret[2], t);
        l += wl * g;
        r += wr * g;
      }
      if (eq_on) {
        const float m = Lerp(prev.eq, next.eq, t);
        float el = l, er = r;
        st.eq.Process(el, er);
        l += (el - l) * m;
        r += (er - r) * m;
      }
      if (comp_on) {
        const float m = Lerp(prev.comp, next.comp, t);
        const float g = st.comp.Gain(std::max(fabsf(l), fabsf(r)));
        l += (l * g - l) * m;
        r += (r * g - r) * m;
        min_gr = std::min(min_gr, st.comp.reduction_gain());
      }
      const float width = Lerp(prev.width, next.width, t);
      if (width != 1.0f) {
        const float mid = (l + r) * 0.5f, side = (l - r) * 0.5f * width;
        l = mid + side;
        r = mid - side;
      }
      const float gain = Lerp(prev.gain, next.gain, t);
      l *= gain;
      r *= gain;
      out[j] = l;
      out[j + 1] = r;
      peak_l = std::max(peak_l, fabsf(l));
      peak_r = std::max(peak_r, fabsf(r));
    }
    if (min_gr < 1.0f) master_meter_.Publish(0, 0, min_gr);
  }
  master_meter_.Publish(peak_l, peak_r, 1.0f);
  if (analyzer_source() < 0)
    WriteScope(out, frames);
}

}
}
}
}
