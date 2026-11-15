#pragma once

// Convolution reverb: uniformly partitioned overlap-save FFT convolution (512-sample partitions,
// 1024-point FFT, so 512 samples = ~11 ms of latency, fine for a reverb send). A mono input is
// convolved with a stereo impulse response.
//
// The impulse response and all the working memory the audio thread needs are built on another
// thread (ConvolutionIr::Build) and handed over whole, so the audio thread never allocates.

#include <complex>
#include <stdint.h>
#include <string>
#include <vector>

namespace dsp {
namespace audio {
namespace synth {
namespace effects {

// In-place radix-2 complex FFT of a fixed size.
class Fft {
 public:
  void Init(int n);
  void Forward(std::complex<float>* data) const { Run(data, false); }
  void Inverse(std::complex<float>* data) const { Run(data, true); }  // unscaled
  int size() const { return n_; }

 private:
  void Run(std::complex<float>* data, bool inverse) const;
  int n_ = 0;
  std::vector<int> bitrev_;
  std::vector<std::complex<float>> twiddle_;
};

struct ConvolutionIr {
  static const int kBlock = 512, kFft = 1024, kBins = kFft / 2 + 1;
  int partitions = 0;
  size_t length = 0;  // samples
  std::string name;
  std::vector<std::complex<float>> spectra[2];  // partitions * kBins per channel
  std::vector<std::complex<float>> history;     // input spectra ring (audio thread working memory)
  // Builds from a stereo impulse response already at the mixer's sample rate.
  static ConvolutionIr* Build(const std::vector<float>& left, const std::vector<float>& right, const std::string& name);
};

class Convolver {
 public:
  Convolver();
  // Audio thread: switch to ir (not owned; nullptr = silence). Clears the running state.
  void SetIr(ConvolutionIr* ir);
  ConvolutionIr* ir() const { return ir_; }
  // One input sample in, one stereo output sample out (delayed by kBlock samples).
  inline void Tick(float in, float& out_l, float& out_r) {
    out_l = out_[0][pos_];
    out_r = out_[1][pos_];
    input_[ConvolutionIr::kBlock + pos_] = in;
    if (++pos_ == ConvolutionIr::kBlock) {
      ProcessBlock();
      pos_ = 0;
    }
  }

 private:
  void ProcessBlock();
  ConvolutionIr* ir_ = nullptr;
  Fft fft_;
  int pos_ = 0, ring_ = 0;
  float input_[ConvolutionIr::kFft];  // previous block + current block
  float out_[2][ConvolutionIr::kBlock];
  std::complex<float> work_[ConvolutionIr::kFft];
  std::complex<float> acc_[ConvolutionIr::kBins];
};

// Synthetic impulse responses so convolution works without files: 0 room, 1 hall, 2 plate.
// Decorrelated noise with frequency-dependent exponential decay plus early reflections.
void MakePresetIr(int preset, uint32_t sample_rate, std::vector<float>& left, std::vector<float>& right);
const char* PresetIrName(int preset);
// Reads a WAV impulse response (PCM 16/24/32-bit or 32-bit float, mono or stereo), resampled to
// sample_rate and limited to max_seconds.
bool ReadImpulseResponse(const char* path, uint32_t sample_rate, double max_seconds, std::vector<float>& left,
                         std::vector<float>& right, std::string* error);

}
}
}
}
