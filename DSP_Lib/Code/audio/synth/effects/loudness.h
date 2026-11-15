#pragma once

// Loudness metering after ITU-R BS.1770 / EBU R128: K-weighted mean square in 100 ms blocks;
// momentary (400 ms), short-term (3 s) and gated integrated loudness in LUFS. Also the stereo
// correlation (+1 mono, 0 unrelated, -1 out of phase).
//
// LoudnessMeter runs where the audio is (it only accumulates and hands 100 ms blocks on);
// LoudnessStats turns blocks into readings on whatever thread shows them.

#include <deque>
#include <stdint.h>
#include <vector>

namespace dsp {
namespace audio {
namespace synth {
namespace effects {

struct LoudnessBlock {
  double mean_square;  // K-weighted, left + right (channel weights 1), over 100 ms
  double lr, ll, rr;   // sums for the correlation
};

class LoudnessMeter {
 public:
  void Configure(uint32_t sample_rate);
  // Interleaved stereo. Completed blocks go into the queue (dropped if nobody empties it).
  void Process(const float* stereo, uint32_t frames);
  bool Pop(LoudnessBlock& block) { return queue_.Pop(block); }

 private:
  struct Biquad {
    double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0, z1[2] = {0, 0}, z2[2] = {0, 0};
    inline double Tick(int ch, double x) {
      const double y = b0 * x + z1[ch];
      z1[ch] = b1 * x - a1 * y + z2[ch];
      z2[ch] = b2 * x - a2 * y;
      return y;
    }
  };
  Biquad shelf_, highpass_;
  uint32_t block_frames_ = 4800, count_ = 0;
  double sum_ = 0, lr_ = 0, ll_ = 0, rr_ = 0;
  SpscQueue<LoudnessBlock, 1024> queue_;
};

class LoudnessStats {
 public:
  void Reset();
  void Add(const LoudnessBlock& block);
  // LUFS; -inf (returned as -144) while there is nothing to measure.
  double momentary() const { return Window(4); }
  double short_term() const { return Window(30); }
  double integrated() const;
  double correlation() const;  // over the last 400 ms
  double seconds() const { return blocks_ * 0.1; }

 private:
  double Window(size_t blocks) const;
  std::deque<LoudnessBlock> recent_;     // last 3 s
  std::vector<double> gating_blocks_;    // 400 ms mean squares, 75% overlap (one per 100 ms)
  size_t blocks_ = 0;
};

}
}
}
}
