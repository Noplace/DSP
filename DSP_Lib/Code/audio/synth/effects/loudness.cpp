#define NOMINMAX
#include <algorithm>
#include <math.h>
#include "../../../dsp.h"

namespace dsp {
namespace audio {
namespace synth {
namespace effects {

// K-weighting: the two BS.1770 stages (high shelf ~+4 dB above ~1.7 kHz, then a ~38 Hz
// high-pass), designed for any rate; at 48 kHz this gives the coefficients in the standard.
void LoudnessMeter::Configure(uint32_t fs) {
  const double kPi = 3.14159265358979323846;
  {
    const double f0 = 1681.974450955533, gain_db = 3.999843853973347, q = 0.7071752369554196;
    const double k = tan(kPi * f0 / fs);
    const double vh = pow(10.0, gain_db / 20.0), vb = pow(vh, 0.4996667741545416);
    const double a0 = 1.0 + k / q + k * k;
    shelf_.b0 = (vh + vb * k / q + k * k) / a0;
    shelf_.b1 = 2.0 * (k * k - vh) / a0;
    shelf_.b2 = (vh - vb * k / q + k * k) / a0;
    shelf_.a1 = 2.0 * (k * k - 1.0) / a0;
    shelf_.a2 = (1.0 - k / q + k * k) / a0;
  }
  {
    const double f0 = 38.13547087602444, q = 0.5003270373238773;
    const double k = tan(kPi * f0 / fs);
    const double a0 = 1.0 + k / q + k * k;
    highpass_.b0 = 1.0;
    highpass_.b1 = -2.0;
    highpass_.b2 = 1.0;
    highpass_.a1 = 2.0 * (k * k - 1.0) / a0;
    highpass_.a2 = (1.0 - k / q + k * k) / a0;
  }
  block_frames_ = std::max<uint32_t>(1, fs / 10);
  count_ = 0;
  sum_ = lr_ = ll_ = rr_ = 0;
}

void LoudnessMeter::Process(const float* stereo, uint32_t frames) {
  for (uint32_t i = 0; i < frames; ++i) {
    const double l = stereo[2 * i], r = stereo[2 * i + 1];
    const double kl = highpass_.Tick(0, shelf_.Tick(0, l));
    const double kr = highpass_.Tick(1, shelf_.Tick(1, r));
    sum_ += kl * kl + kr * kr;
    lr_ += l * r;
    ll_ += l * l;
    rr_ += r * r;
    if (++count_ == block_frames_) {
      queue_.Push({sum_ / count_, lr_, ll_, rr_});
      count_ = 0;
      sum_ = lr_ = ll_ = rr_ = 0;
    }
  }
}

static double ToLufs(double mean_square) {
  return mean_square > 1e-15 ? -0.691 + 10.0 * log10(mean_square) : -144.0;
}

void LoudnessStats::Reset() {
  recent_.clear();
  gating_blocks_.clear();
  blocks_ = 0;
}

void LoudnessStats::Add(const LoudnessBlock& block) {
  recent_.push_back(block);
  if (recent_.size() > 30) recent_.pop_front();
  ++blocks_;
  if (recent_.size() >= 4) {  // a 400 ms gating block every 100 ms (75% overlap)
    double ms = 0;
    for (size_t i = recent_.size() - 4; i < recent_.size(); ++i) ms += recent_[i].mean_square;
    gating_blocks_.push_back(ms / 4.0);
  }
}

double LoudnessStats::Window(size_t blocks) const {
  if (recent_.size() < blocks) return -144.0;
  double ms = 0;
  for (size_t i = recent_.size() - blocks; i < recent_.size(); ++i) ms += recent_[i].mean_square;
  return ToLufs(ms / blocks);
}

// Two gates: absolute -70 LUFS, then relative 10 LU below the level of what passed the first.
double LoudnessStats::integrated() const {
  double sum = 0;
  size_t n = 0;
  for (double ms : gating_blocks_)
    if (ToLufs(ms) > -70.0) { sum += ms; ++n; }
  if (n == 0) return -144.0;
  const double relative = ToLufs(sum / n) - 10.0;
  sum = 0;
  n = 0;
  for (double ms : gating_blocks_)
    if (ToLufs(ms) > -70.0 && ToLufs(ms) > relative) { sum += ms; ++n; }
  return n ? ToLufs(sum / n) : -144.0;
}

double LoudnessStats::correlation() const {
  double lr = 0, ll = 0, rr = 0;
  const size_t from = recent_.size() > 4 ? recent_.size() - 4 : 0;
  for (size_t i = from; i < recent_.size(); ++i) {
    lr += recent_[i].lr;
    ll += recent_[i].ll;
    rr += recent_[i].rr;
  }
  const double d = sqrt(ll * rr);
  return d > 1e-12 ? lr / d : 0.0;
}

}
}
}
}
