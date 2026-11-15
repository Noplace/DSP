#pragma once

#include <atomic>
#include <stdint.h>
#include <vector>

namespace dsp {
namespace audio {
namespace synth {
namespace effects {

// ---------------------------------------------------------------------------------------------
// Parameters
//
// Every mixer parameter is a float in an atomic slot. The UI thread writes them at any time
// without locking; the audio thread reads them once per 32-frame sub-block, smooths them and
// recomputes filter coefficients when they move, so dragging a control never blocks or clicks.
// ---------------------------------------------------------------------------------------------

enum class ParamUnit : uint8_t { kNone, kDb, kFader, kHz, kMs, kPercent, kRatio, kPan, kSwitch, kQ, kTimes };

struct ParamInfo {
  const char* name;  // short label
  float min, max, def;
  ParamUnit unit;
  bool log;          // control travel is logarithmic (frequencies, times, ratios)
};

// Control position (0..1) <-> value.
float ParamToNormalized(const ParamInfo& info, float value);
float ParamFromNormalized(const ParamInfo& info, float t);
// Value as display text, e.g. "-3.0 dB", "1.2 kHz", "4.0:1", "L20".
void FormatParam(const ParamInfo& info, float value, char* out, size_t out_size);

// 4-band parametric EQ: low shelf, two peaks, high shelf.
enum EqParam {
  kEqOn,
  kEqLowFreq, kEqLowGain, kEqLowQ,
  kEqMid1Freq, kEqMid1Gain, kEqMid1Q,
  kEqMid2Freq, kEqMid2Gain, kEqMid2Q,
  kEqHighFreq, kEqHighGain, kEqHighQ,
  kEqParamCount
};
static const int kEqBands = 4;
inline int EqBandFreq(int band) { return kEqLowFreq + band * 3; }
inline int EqBandGain(int band) { return kEqLowGain + band * 3; }
inline int EqBandQ(int band) { return kEqLowQ + band * 3; }

// Feed-forward compressor with soft knee.
enum CompParam { kCompOn, kCompThreshold, kCompRatio, kCompAttack, kCompRelease, kCompKnee, kCompMakeup, kCompParamCount };

// One channel strip. Signal flow:
//   PRE    trim/phase -> high-pass -> drive -> low-pass
//   EQ     4 bands               } order swappable (kStripDynFirst)
//   DYN    compressor            }
//   POST   fader -> balance -> mute/solo, sends (pre- or post-fader) to the reverb/delay/chorus buses
enum StripParam {
  kStripTrim, kStripPhase,
  kStripHpfOn, kStripHpfFreq, kStripHpfSteep,
  kStripLpfOn, kStripLpfFreq, kStripLpfQ,
  kStripDriveOn, kStripDrive, kStripDriveMix,
  kStripEq,                                     // kEqParamCount EQ parameters
  kStripComp = kStripEq + kEqParamCount,        // kCompParamCount compressor parameters
  kStripDynFirst = kStripComp + kCompParamCount,
  kStripVolume, kStripPan, kStripMute, kStripSolo,
  kStripSendPre, kStripSendReverb, kStripSendDelay, kStripSendChorus,
  kStripParamCount
};

// Master bus. Signal flow:
//   sum of strips + FX returns (reverb, delay, chorus) -> master EQ -> bus compressor
//   -> stereo width -> output gain   (the player's peak limiter follows)
enum MasterParam {
  kRevSize, kRevDamp, kRevWidth, kRevPreDelay, kRevReturn,
  kDlyTime, kDlyFeedback, kDlyDamp, kDlyPingPong, kDlyReturn,
  kChoRate, kChoDepth, kChoReturn,
  kMasterEq,                                    // kEqParamCount EQ parameters
  kMasterComp = kMasterEq + kEqParamCount,      // kCompParamCount compressor parameters
  kMasterWidth = kMasterComp + kCompParamCount,
  kMasterGain,
  kMasterParamCount
};

const ParamInfo& StripParamInfo(int id);
const ParamInfo& MasterParamInfo(int id);
// Stable names for session/preset files ("eq.lomid.gain", "reverb.size", ...); -1 if unknown.
const char* StripParamKey(int id);
const char* MasterParamKey(int id);
int FindStripParam(const char* key);
int FindMasterParam(const char* key);

template <int N>
class ParamBank {
 public:
  float get(int id) const { return values_[id].load(std::memory_order_relaxed); }
  void set(int id, float v) { values_[id].store(v, std::memory_order_relaxed); }
  std::atomic<float>* slot(int id) { return &values_[id]; }
 private:
  std::atomic<float> values_[N] = {};
};
using StripParams = ParamBank<kStripParamCount>;
using MasterParams = ParamBank<kMasterParamCount>;

// Response of an EQ described by params[base .. base + kEqParamCount) at freq, in dB
// (ignores the on/off switch). Used by the UI to draw the curve.
double EqResponseDb(const float* eq_params, double sample_rate, double freq);

// ---------------------------------------------------------------------------------------------
// Mixer
// ---------------------------------------------------------------------------------------------
class Mixer {
 public:
  static const int kStrips = 16;
  static const int kSubBlock = 32;
  static const int kScopeSize = 8192;  // analyser ring buffer (power of two)

  Mixer();
  ~Mixer();
  // max_frames: largest block Process() will be asked for.
  void Initialize(uint32_t sample_rate, uint32_t max_frames);
  void Deinitialize();
  uint32_t sample_rate() const { return sample_rate_; }

  // Audio thread. strip_in[i] = interleaved stereo of channel i (processed in place);
  // out = interleaved stereo master mix.
  void Process(float* const* strip_in, uint32_t frames, float* out);

  StripParams& strip(int i) { return strip_params_[i]; }
  MasterParams& master() { return master_params_; }
  void ResetStrip(int i);
  void ResetMaster();
  void ResetAll();
  bool AnySolo() const;

  // Send levels a MIDI file asks for (GM CC91 reverb, CC93 chorus), set by the synth. When
  // following them, a strip sends its own send knob plus the file's level.
  enum Bus { kBusReverb = 0, kBusDelay = 1, kBusChorus = 2 };
  void set_midi_send(int strip, int bus, float level) { midi_send_[strip][bus].store(level, std::memory_order_relaxed); }
  float midi_send(int strip, int bus) const { return midi_send_[strip][bus].load(std::memory_order_relaxed); }
  void set_follow_midi_sends(bool on) { follow_midi_sends_.store(on, std::memory_order_relaxed); }
  bool follow_midi_sends() const { return follow_midi_sends_.load(std::memory_order_relaxed); }
  // Send level actually used (knob + file level when following), 0..
  float EffectiveSend(int strip, int bus) const;

  // Meters (UI thread): peak since the last call, and the largest gain reduction (as a gain).
  void TakeStripPeak(int i, float& l, float& r) {
    l = strips_meter_[i].peak[0].exchange(0.0f);
    r = strips_meter_[i].peak[1].exchange(0.0f);
  }
  float TakeStripGainReduction(int i) { return strips_meter_[i].gr.exchange(1.0f); }
  void TakeMasterPeak(float& l, float& r) {
    l = master_meter_.peak[0].exchange(0.0f);
    r = master_meter_.peak[1].exchange(0.0f);
  }
  float TakeMasterGainReduction() { return master_meter_.gr.exchange(1.0f); }

  // Spectrum/scope tap: -1 = master output, 0..15 = that strip after its fader.
  void set_analyzer_source(int source) { analyzer_source_.store(source, std::memory_order_relaxed); }
  int analyzer_source() const { return analyzer_source_.load(std::memory_order_relaxed); }
  // Copies the most recent `count` mono samples of the tap (oldest first). count <= kScopeSize.
  void ReadScope(float* dest, int count) const;

 private:
  struct Meter {
    std::atomic<float> peak[2];
    std::atomic<float> gr;
    Meter() { peak[0] = peak[1] = 0.0f; gr = 1.0f; }
    void Publish(float l, float r, float gain);
  };
  struct StripState;
  struct MasterState;

  void ProcessStrip(int index, float* io, uint32_t frames, float* out, bool any_solo);
  void ProcessMaster(float* out, uint32_t frames);
  void WriteScope(const float* stereo, uint32_t frames);

  uint32_t sample_rate_ = 0;
  uint32_t max_frames_ = 0;
  StripParams strip_params_[kStrips];
  MasterParams master_params_;
  Meter strips_meter_[kStrips];
  Meter master_meter_;
  StripState* strips_ = nullptr;
  MasterState* master_ = nullptr;
  std::vector<float> bus_[3];  // reverb, delay, chorus send buses (interleaved stereo)
  std::atomic<float> midi_send_[kStrips][3] = {};
  std::atomic<bool> follow_midi_sends_{false};
  std::atomic<float> scope_[kScopeSize];
  std::atomic<uint32_t> scope_pos_;
  std::atomic<int> analyzer_source_;
};

}
}
}
}
