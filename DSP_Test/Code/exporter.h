#pragma once

// Renders the loaded song to a WAV file on a worker thread, with its own synth instance set up
// like the live one (mixer, instrument overrides, track mute/solo, file sends), so playback
// keeps going while it runs. Output: 24-bit PCM stereo through the same peak limiter.

#include <atomic>
#include <string>
#include <thread>

namespace demo {

struct ExportSettings {
  std::string midi_path, wav_path;
  uint32_t sample_rate = 48000;
  float strip[16][dsp::audio::synth::effects::kStripParamCount];
  float master[dsp::audio::synth::effects::kMasterParamCount];
  int channel_override[16];
  bool follow_midi_sends = false;
  bool track_mute[64], track_solo[64];
  dsp::audio::synth::SongInfo song;  // the song as it is now (edits included), rendered at speed 1, no loop
  // Copies everything above (except the paths) from the live synth.
  void CaptureFrom(dsp::audio::synth::MidiSynth& synth);
};

class Exporter {
 public:
  enum State { kIdle, kPreparing, kRendering, kDone, kFailed, kCancelled };
  ~Exporter() { Cancel(); Join(); }
  bool Start(const ExportSettings& settings);
  void Cancel() { cancel_ = true; }
  bool busy() const { int s = state_; return s == kPreparing || s == kRendering; }
  State state() const { return State(state_.load()); }
  float progress() const { return progress_; }  // 0..1
  // Result text once finished ("song.wav  3:02  peak -1.0 dBFS" or the error).
  std::string result() const;
  void Acknowledge();  // back to idle after showing the result

 private:
  void Run(ExportSettings settings);
  void Join() { if (thread_.joinable()) thread_.join(); }
  std::thread thread_;
  std::atomic<int> state_{kIdle};
  std::atomic<float> progress_{0};
  std::atomic<bool> cancel_{false};
  std::string result_;  // written by the worker before it sets a final state
};

}
