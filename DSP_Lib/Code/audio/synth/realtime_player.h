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

#include <atomic>
#include <mutex>

namespace dsp {
namespace audio {
namespace synth {

// Drives a Synth from the WASAPI audio callback (pull model, low latency).
//
// Threading:
//  - Play/Pause/Stop only set atomics; the audio thread applies them at the start of the
//    next buffer. Stop() is also safe to call from the audio thread (the synth does that at
//    the end of a song).
//  - Anything that changes synth data from another thread (e.g. loading a MIDI file) must go
//    through WithSynthLocked(). The audio thread only ever try_locks, so it never blocks: if
//    the UI holds the lock it outputs silence for that buffer.
class RealtimePlayer : public SynthHost {
 public:
  enum State { kStateStopped = 0, kStatePlaying = 1, kStatePaused = 2 };

  RealtimePlayer();
  ~RealtimePlayer();

  // Opens the default audio device, sets the synth's sample rate to the device rate and
  // initializes it. Returns false if the device could not be opened (see last_error()).
  bool Initialize(Synth* synth);
  void Deinitialize();

  void Play();
  void Pause();
  void Stop() override;
  void TogglePause();

  template <typename F>
  void WithSynthLocked(F&& f) {
    std::lock_guard<std::mutex> lock(synth_lock_);
    f();
  }

  State state() const { return static_cast<State>(state_.load()); }
  double GetPlaybackSeconds() const;
  const output::WasapiOutput& output() const { return output_; }
  const char* last_error() const { return output_.last_error(); }

 private:
  static void RenderThunk(void* user, float* stereo_out, uint32_t frames);
  void Render(float* stereo_out, uint32_t frames);

  output::WasapiOutput output_;
  Synth* synth_;
  std::mutex synth_lock_;
  std::atomic<int> desired_state_;
  std::atomic<int> state_;
  std::atomic<bool> rewind_requested_;
  std::atomic<bool> ready_;
  std::atomic<uint64_t> frames_played_;
};

}
}
}
