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
#pragma once

#include <math.h>

namespace dsp {
namespace audio {
namespace synth {
namespace effects {

// Stereo-linked peak limiter with instant attack: the envelope jumps to any new peak, so
// |output| never exceeds the threshold, then decays exponentially and the gain recovers
// smoothly.
class MasterLimiter {
 public:
  MasterLimiter() : threshold_(0.891f), release_coef_(0.9999f), envelope_(0.0f) {}  // -1 dBFS

  void Configure(uint32_t sample_rate, float threshold = 0.891f, float release_ms = 150.0f) {
    threshold_ = threshold;
    release_coef_ = expf(-1.0f / (release_ms * 0.001f * sample_rate));
  }
  void Reset() { envelope_ = 0.0f; }

  // Processes interleaved stereo in place; returns the smallest gain applied (1 = untouched).
  float Process(float* stereo, uint32_t frames) {
    float env = envelope_;
    float min_gain = 1.0f;
    for (uint32_t i = 0; i < frames; ++i) {
      float& l = stereo[i * 2];
      float& r = stereo[i * 2 + 1];
      float peak = fabsf(l) > fabsf(r) ? fabsf(l) : fabsf(r);
      env = peak > env ? peak : env * release_coef_;
      if (env > threshold_) {
        float gain = threshold_ / env;
        l *= gain;
        r *= gain;
        if (gain < min_gain) min_gain = gain;
      }
    }
    envelope_ = env;
    return min_gain;
  }

 private:
  float threshold_, release_coef_, envelope_;
};

}
}
}
}
