#define NOMINMAX
#include <algorithm>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "convolver.h"

namespace dsp {
namespace audio {
namespace synth {
namespace effects {

// ---------------------------------------------------------------------------------------------
// FFT
// ---------------------------------------------------------------------------------------------
void Fft::Init(int n) {
  n_ = n;
  bitrev_.resize(n);
  int bits = 0;
  while ((1 << bits) < n) ++bits;
  for (int i = 0; i < n; ++i) {
    int r = 0;
    for (int b = 0; b < bits; ++b) r |= ((i >> b) & 1) << (bits - 1 - b);
    bitrev_[i] = r;
  }
  twiddle_.resize(n / 2);
  for (int i = 0; i < n / 2; ++i)
    twiddle_[i] = std::polar(1.0f, float(-2.0 * 3.14159265358979323846 * i / n));
}

void Fft::Run(std::complex<float>* a, bool inverse) const {
  for (int i = 0; i < n_; ++i)
    if (i < bitrev_[i]) std::swap(a[i], a[bitrev_[i]]);
  for (int len = 2; len <= n_; len <<= 1) {
    const int half = len / 2, step = n_ / len;
    for (int i = 0; i < n_; i += len)
      for (int k = 0; k < half; ++k) {
        std::complex<float> w = twiddle_[k * step];
        if (inverse) w = std::conj(w);
        const std::complex<float> t = a[i + k + half] * w;
        a[i + k + half] = a[i + k] - t;
        a[i + k] += t;
      }
  }
}

// ---------------------------------------------------------------------------------------------
// Impulse response
// ---------------------------------------------------------------------------------------------
ConvolutionIr* ConvolutionIr::Build(const std::vector<float>& left, const std::vector<float>& right,
                                    const std::string& name) {
  auto ir = new ConvolutionIr();
  ir->name = name;
  ir->length = std::max(left.size(), right.size());
  ir->partitions = std::max(1, int((ir->length + kBlock - 1) / kBlock));
  Fft fft;
  fft.Init(kFft);
  std::vector<std::complex<float>> work(kFft);
  const std::vector<float>* src[2] = {&left, right.empty() ? &left : &right};
  for (int ch = 0; ch < 2; ++ch) {
    ir->spectra[ch].resize(size_t(ir->partitions) * kBins);
    for (int p = 0; p < ir->partitions; ++p) {
      std::fill(work.begin(), work.end(), std::complex<float>(0, 0));
      for (int i = 0; i < kBlock; ++i) {
        const size_t s = size_t(p) * kBlock + i;
        if (s < src[ch]->size()) work[i] = (*src[ch])[s];
      }
      fft.Forward(work.data());
      std::copy(work.begin(), work.begin() + kBins, ir->spectra[ch].begin() + size_t(p) * kBins);
    }
  }
  ir->history.assign(size_t(ir->partitions) * kBins, std::complex<float>(0, 0));
  return ir;
}

// ---------------------------------------------------------------------------------------------
// Convolver
// ---------------------------------------------------------------------------------------------
Convolver::Convolver() {
  fft_.Init(ConvolutionIr::kFft);
  memset(input_, 0, sizeof(input_));
  memset(out_, 0, sizeof(out_));
}

void Convolver::SetIr(ConvolutionIr* ir) {
  ir_ = ir;
  pos_ = 0;
  ring_ = 0;
  memset(input_, 0, sizeof(input_));
  memset(out_, 0, sizeof(out_));
  if (ir_) std::fill(ir_->history.begin(), ir_->history.end(), std::complex<float>(0, 0));
}

void Convolver::ProcessBlock() {
  const int B = ConvolutionIr::kBlock, N = ConvolutionIr::kFft, K = ConvolutionIr::kBins;
  if (!ir_) {
    memset(out_, 0, sizeof(out_));
    memmove(input_, input_ + B, sizeof(float) * B);
    return;
  }
  // spectrum of the last 2B input samples
  for (int i = 0; i < N; ++i) work_[i] = input_[i];
  fft_.Forward(work_);
  const int P = ir_->partitions;
  std::complex<float>* slot = ir_->history.data() + size_t(ring_) * K;
  std::copy(work_, work_ + K, slot);
  for (int ch = 0; ch < 2; ++ch) {
    std::fill(acc_, acc_ + K, std::complex<float>(0, 0));
    const std::complex<float>* h = ir_->spectra[ch].data();
    for (int p = 0; p < P; ++p) {
      int r = ring_ - p;
      if (r < 0) r += P;
      const std::complex<float>* x = ir_->history.data() + size_t(r) * K;
      const std::complex<float>* hp = h + size_t(p) * K;
      for (int k = 0; k < K; ++k) acc_[k] += x[k] * hp[k];
    }
    // real signal: rebuild the upper half by conjugate symmetry, then back to time
    for (int k = 0; k < K; ++k) work_[k] = acc_[k];
    for (int k = K; k < N; ++k) work_[k] = std::conj(acc_[N - k]);
    fft_.Inverse(work_);
    const float scale = 1.0f / N;
    for (int i = 0; i < B; ++i) out_[ch][i] = work_[B + i].real() * scale;  // overlap-save: last B
  }
  if (++ring_ >= P) ring_ = 0;
  memmove(input_, input_ + B, sizeof(float) * B);
}

// ---------------------------------------------------------------------------------------------
// Built-in impulse responses
// ---------------------------------------------------------------------------------------------
const char* PresetIrName(int preset) {
  static const char* kNames[3] = {"Room", "Hall", "Plate"};
  return kNames[std::clamp(preset, 0, 2)];
}

void MakePresetIr(int preset, uint32_t fs, std::vector<float>& left, std::vector<float>& right) {
  struct Shape {
    float rt60, hf_rt60, build_ms, er_ms;
    int er_count;
  };
  static const Shape kShapes[3] = {
    {0.75f, 0.35f, 3.0f, 28.0f, 10},   // room: short, quickly darkening, dense early reflections
    {2.4f, 1.1f, 18.0f, 70.0f, 16},    // hall: long, warm, sparse early reflections
    {1.7f, 1.4f, 1.0f, 0.0f, 0},       // plate: bright, immediate, no early reflections
  };
  const Shape& s = kShapes[std::clamp(preset, 0, 2)];
  const size_t n = size_t(s.rt60 * 1.15f * fs);
  left.assign(n, 0.0f);
  right.assign(n, 0.0f);
  uint32_t seed[2] = {0x1234567u + uint32_t(preset) * 977u, 0x89abcdefu + uint32_t(preset) * 131u};
  const float split = 1.0f - expf(-6.2831853f * 2500.0f / fs);  // low/high split for the decay
  for (int ch = 0; ch < 2; ++ch) {
    std::vector<float>& out = ch ? right : left;
    float lp = 0;
    for (size_t i = 0; i < n; ++i) {
      seed[ch] ^= seed[ch] << 13;
      seed[ch] ^= seed[ch] >> 17;
      seed[ch] ^= seed[ch] << 5;
      const float noise = (seed[ch] / 4294967296.0f) * 2.0f - 1.0f;
      lp += (noise - lp) * split;
      const float t = float(i) / fs;
      const float body = lp * expf(-6.908f * t / s.rt60) + (noise - lp) * expf(-6.908f * t / s.hf_rt60);
      const float build = s.build_ms > 0 ? std::min(1.0f, t * 1000.0f / s.build_ms) : 1.0f;
      out[i] = body * build * 0.6f;
    }
    // early reflections: a few distinct taps, a little different on each side
    for (int e = 0; e < s.er_count; ++e) {
      seed[ch] = seed[ch] * 1664525u + 1013904223u;
      const float when = 3.0f + (s.er_ms - 3.0f) * (seed[ch] / 4294967296.0f);
      const size_t at = size_t(when * 0.001f * fs);
      if (at >= n) continue;
      const float amp = (0.9f - 0.6f * e / std::max(1, s.er_count)) * ((e & 1) ? -1.0f : 1.0f);
      out[at] += amp;
    }
  }
  // Same loudness for every preset (and about the algorithmic reverb's level at the same return).
  double energy = 0;
  for (size_t i = 0; i < n; ++i) energy += double(left[i]) * left[i] + double(right[i]) * right[i];
  const float g = float(sqrt(2.5 / std::max(energy, 1e-9)));
  for (size_t i = 0; i < n; ++i) {
    left[i] *= g;
    right[i] *= g;
  }
}

// ---------------------------------------------------------------------------------------------
// WAV impulse responses
// ---------------------------------------------------------------------------------------------
bool ReadImpulseResponse(const char* path, uint32_t fs, double max_seconds, std::vector<float>& left,
                         std::vector<float>& right, std::string* error) {
  auto fail = [&](const char* why) {
    if (error) *error = why;
    return false;
  };
  FILE* f = nullptr;
  if (fopen_s(&f, path, "rb") != 0 || !f) return fail("cannot open the file");
  std::vector<uint8_t> data;
  fseek(f, 0, SEEK_END);
  const long size = ftell(f);
  fseek(f, 0, SEEK_SET);
  if (size > 0 && size < 512L * 1024 * 1024) {
    data.resize(size_t(size));
    if (fread(data.data(), 1, data.size(), f) != data.size()) data.clear();
  }
  fclose(f);
  if (data.size() < 44 || memcmp(data.data(), "RIFF", 4) != 0 || memcmp(data.data() + 8, "WAVE", 4) != 0)
    return fail("not a WAV file");
  uint16_t format = 0, channels = 0, bits = 0;
  uint32_t rate = 0;
  const uint8_t* samples = nullptr;
  size_t sample_bytes = 0;
  for (size_t pos = 12; pos + 8 <= data.size();) {
    const uint32_t len = *reinterpret_cast<const uint32_t*>(&data[pos + 4]);
    const uint8_t* body = &data[pos + 8];
    if (pos + 8 + len > data.size()) break;
    if (memcmp(&data[pos], "fmt ", 4) == 0 && len >= 16) {
      format = *reinterpret_cast<const uint16_t*>(body);
      channels = *reinterpret_cast<const uint16_t*>(body + 2);
      rate = *reinterpret_cast<const uint32_t*>(body + 4);
      bits = *reinterpret_cast<const uint16_t*>(body + 14);
      if (format == 0xFFFE && len >= 26) format = *reinterpret_cast<const uint16_t*>(body + 24);  // extensible
    } else if (memcmp(&data[pos], "data", 4) == 0) {
      samples = body;
      sample_bytes = len;
    }
    pos += 8 + len + (len & 1);
  }
  if (!samples || channels == 0 || rate == 0) return fail("no audio data found");
  if (!((format == 1 && (bits == 16 || bits == 24 || bits == 32)) || (format == 3 && bits == 32)))
    return fail("unsupported WAV format (use PCM 16/24/32-bit or 32-bit float)");
  const size_t frame_bytes = size_t(channels) * bits / 8;
  const size_t frames = sample_bytes / frame_bytes;
  auto read = [&](size_t frame, int ch) -> float {
    const uint8_t* p = samples + frame * frame_bytes + size_t(std::min<int>(ch, channels - 1)) * bits / 8;
    if (format == 3) return *reinterpret_cast<const float*>(p);
    if (bits == 16) return *reinterpret_cast<const int16_t*>(p) / 32768.0f;
    if (bits == 24) return int32_t(uint32_t(p[0]) << 8 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 24) / 2147483648.0f;
    return *reinterpret_cast<const int32_t*>(p) / 2147483648.0f;
  };
  // resample (linear) to the mixer rate and trim
  const double ratio = double(rate) / fs;
  size_t out_frames = size_t(frames / ratio);
  out_frames = std::min(out_frames, size_t(max_seconds * fs));
  left.resize(out_frames);
  right.resize(out_frames);
  for (size_t i = 0; i < out_frames; ++i) {
    const double src = i * ratio;
    const size_t a = std::min(frames - 1, size_t(src));
    const size_t b = std::min(frames - 1, a + 1);
    const float t = float(src - a);
    left[i] = read(a, 0) + (read(b, 0) - read(a, 0)) * t;
    right[i] = read(a, 1) + (read(b, 1) - read(a, 1)) * t;
  }
  // trim the silent tail and normalise to the presets' energy
  size_t end = out_frames;
  while (end > 0 && fabsf(left[end - 1]) < 1e-5f && fabsf(right[end - 1]) < 1e-5f) --end;
  left.resize(end);
  right.resize(end);
  if (end == 0) return fail("the impulse response is silent");
  double energy = 0;
  for (size_t i = 0; i < end; ++i) energy += double(left[i]) * left[i] + double(right[i]) * right[i];
  const float g = float(sqrt(2.5 / std::max(energy, 1e-12)));
  for (size_t i = 0; i < end; ++i) {
    left[i] *= g;
    right[i] *= g;
  }
  return true;
}

}
}
}
}
