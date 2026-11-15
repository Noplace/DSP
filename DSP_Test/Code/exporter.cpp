#include "global.h"

namespace demo {

using namespace dsp::audio::synth;
using namespace dsp::audio::synth::effects;

void ExportSettings::CaptureFrom(MidiSynth& synth) {
  for (int s = 0; s < 16; ++s)
    for (int id = 0; id < kStripParamCount; ++id) strip[s][id] = synth.mixer.strip(s).get(id);
  for (int id = 0; id < kMasterParamCount; ++id) master[id] = synth.mixer.master().get(id);
  for (int ch = 0; ch < 16; ++ch) channel_override[ch] = synth.channel_override(ch);
  follow_midi_sends = synth.mixer.follow_midi_sends();
  for (int t = 0; t < 64; ++t) {
    track_mute[t] = synth.track_muted(t);
    track_solo[t] = synth.track_solo(t);
  }
  song = synth.song();
}

bool Exporter::Start(const ExportSettings& settings) {
  if (busy()) return false;
  Join();
  cancel_ = false;
  progress_ = 0;
  result_.clear();
  state_ = kPreparing;
  thread_ = std::thread(&Exporter::Run, this, settings);
  return true;
}

std::string Exporter::result() const {
  const int s = state_;
  return s == kDone || s == kFailed || s == kCancelled ? result_ : std::string();
}

void Exporter::Acknowledge() {
  if (!busy()) {
    Join();
    state_ = kIdle;
  }
}

namespace {

// 24-bit PCM WAV, streamed: the header's sizes are patched in when the file is finished.
class WavWriter {
 public:
  bool Open(const char* path, uint32_t rate) {
    if (fopen_s(&f_, path, "wb") != 0 || !f_) return false;
    const uint16_t channels = 2, bits = 24, align = channels * bits / 8;
    const uint32_t byte_rate = rate * align, fmt_size = 16, zero = 0;
    const uint16_t pcm = 1;
    fwrite("RIFF", 1, 4, f_); fwrite(&zero, 4, 1, f_); fwrite("WAVE", 1, 4, f_);
    fwrite("fmt ", 1, 4, f_); fwrite(&fmt_size, 4, 1, f_); fwrite(&pcm, 2, 1, f_); fwrite(&channels, 2, 1, f_);
    fwrite(&rate, 4, 1, f_); fwrite(&byte_rate, 4, 1, f_); fwrite(&align, 2, 1, f_); fwrite(&bits, 2, 1, f_);
    fwrite("data", 1, 4, f_); fwrite(&zero, 4, 1, f_);
    return true;
  }
  // Interleaved stereo floats; TPDF dither at the 24-bit LSB.
  void Write(const float* stereo, uint32_t frames) {
    buffer_.resize(size_t(frames) * 6);
    uint8_t* p = buffer_.data();
    for (uint32_t i = 0; i < frames * 2; ++i) {
      const float dither = (Random() + Random() - 1.0f) / 8388608.0f;
      const float v = std::clamp(stereo[i] + dither, -1.0f, 1.0f);
      const int32_t s = int32_t(lrintf(v * 8388607.0f));
      *p++ = uint8_t(s);
      *p++ = uint8_t(s >> 8);
      *p++ = uint8_t(s >> 16);
    }
    fwrite(buffer_.data(), 1, buffer_.size(), f_);
    data_bytes_ += uint32_t(buffer_.size());
  }
  bool Close() {
    if (!f_) return false;
    const uint32_t riff = 36 + data_bytes_;
    fseek(f_, 4, SEEK_SET); fwrite(&riff, 4, 1, f_);
    fseek(f_, 40, SEEK_SET); fwrite(&data_bytes_, 4, 1, f_);
    const bool ok = fflush(f_) == 0 && !ferror(f_);
    fclose(f_);
    f_ = nullptr;
    return ok;
  }
  ~WavWriter() { if (f_) fclose(f_); }

 private:
  float Random() {
    seed_ = seed_ * 1664525u + 1013904223u;
    return (seed_ >> 8) / 16777216.0f;
  }
  FILE* f_ = nullptr;
  uint32_t data_bytes_ = 0;
  uint32_t seed_ = 12345;
  std::vector<uint8_t> buffer_;
};

class OfflineHost : public SynthHost {
 public:
  bool stopped = false;
  void Stop() override { stopped = true; }
};

}  // namespace

void Exporter::Run(ExportSettings s) {
  auto finish = [&](State state, const std::string& text) {
    result_ = text;
    state_ = state;
  };
  OfflineHost host;
  MidiSynth synth;
  synth.player_ = &host;
  synth.set_sample_rate(s.sample_rate);
  for (int ch = 0; ch < 16; ++ch) synth.set_channel_override(ch, s.channel_override[ch]);
  synth.Initialize();  // builds the instrument bank: the slow part
  if (cancel_) return finish(kCancelled, "Export cancelled");

  for (int st = 0; st < 16; ++st)
    for (int id = 0; id < kStripParamCount; ++id) synth.mixer.strip(st).set(id, s.strip[st][id]);
  for (int id = 0; id < kMasterParamCount; ++id) synth.mixer.master().set(id, s.master[id]);
  synth.mixer.set_follow_midi_sends(s.follow_midi_sends);
  synth.InstallSong(MidiSynth::PrepareSong(s.song), -1.0);  // the song as edited, not the file
  host.stopped = false;  // installing stops the (offline) transport
  const double length_ms = host.song_length_ms;
  if (length_ms <= 0) return finish(kFailed, "Export failed: the song has no notes");
  for (int t = 0; t < 64; ++t) {
    synth.set_track_muted(t, s.track_mute[t]);
    synth.set_track_solo(t, s.track_solo[t]);
  }

  WavWriter wav;
  if (!wav.Open(s.wav_path.c_str(), s.sample_rate))
    return finish(kFailed, "Export failed: cannot write " + s.wav_path);
  state_ = kRendering;

  MasterLimiter limiter;
  limiter.Configure(s.sample_rate);
  const uint32_t kBlock = 512;
  float block[kBlock * 2];
  float peak = 0;
  uint64_t frames = 0;
  const uint64_t max_frames = uint64_t((length_ms / 1000.0 + 30.0) * s.sample_rate);
  while (!host.stopped && !cancel_ && frames < max_frames) {
    synth.RenderSamplesStereo(kBlock, block);
    limiter.Process(block, kBlock);
    for (float v : block) peak = std::max(peak, fabsf(v));
    wav.Write(block, kBlock);
    frames += kBlock;
    progress_ = float(std::min(0.99, frames * 1000.0 / s.sample_rate / length_ms));
  }
  // Release tail: until the reverb/delay have died away (0.5 s below -90 dBFS), at most 20 s.
  synth.AllNotesOff();
  uint32_t quiet = 0;
  for (uint64_t tail = 0; !cancel_ && tail < uint64_t(20) * s.sample_rate && quiet < s.sample_rate / 2; tail += kBlock) {
    synth.RenderIdleStereo(kBlock, block);
    limiter.Process(block, kBlock);
    float block_peak = 0;
    for (float v : block) block_peak = std::max(block_peak, fabsf(v));
    peak = std::max(peak, block_peak);
    quiet = block_peak < 3.2e-5f ? quiet + kBlock : 0;
    wav.Write(block, kBlock);
    frames += kBlock;
  }
  const bool closed = wav.Close();
  if (cancel_) {
    DeleteFileA(s.wav_path.c_str());
    return finish(kCancelled, "Export cancelled");
  }
  if (!closed) return finish(kFailed, "Export failed while writing " + s.wav_path);

  const char* name = s.wav_path.c_str();
  for (const char* c = name; *c; ++c)
    if (*c == '\\' || *c == '/') name = c + 1;
  const int secs = int(frames / s.sample_rate);
  char text[512];
  snprintf(text, sizeof(text), "Exported %s  \xC2\xB7  %d:%02d  \xC2\xB7  peak %.1f dBFS", name, secs / 60, secs % 60,
           20.0f * log10f(std::max(peak, 1e-6f)));
  progress_ = 1.0f;
  finish(kDone, text);
}

}
