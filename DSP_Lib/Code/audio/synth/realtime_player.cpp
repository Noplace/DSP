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
#include "../../dsp.h"

namespace dsp {
namespace audio {
namespace synth {

RealtimePlayer::RealtimePlayer()
    : synth_(nullptr), midi_input_(nullptr), peak_level_(0.0f), limiter_threshold_(0.891f),  // -1 dBFS
      limiter_release_coef_(0.9999f), limiter_envelope_(0.0f), limiter_min_gain_(1.0f),
      desired_state_(kStateStopped), state_(kStateStopped), rewind_requested_(false), ready_(false),
      frames_played_(0) {
}

RealtimePlayer::~RealtimePlayer() {
  Deinitialize();
}

bool RealtimePlayer::Initialize(Synth* synth) {
  if (ready_)
    return true;
  synth_ = synth;
  synth_->player_ = this;

  // The callback outputs silence until ready_ is set, so it's fine that the device starts first:
  // we need its sample rate before the synth can be initialized.
  if (!output_.Start(&RealtimePlayer::RenderThunk, this))
    return false;

  WithSynthLocked([&] {
    synth_->set_sample_rate(output_.sample_rate());
    synth_->Initialize();
  });
  // Limiter recovers with a ~150 ms time constant.
  limiter_release_coef_ = expf(-1.0f / (0.150f * output_.sample_rate()));
  ready_ = true;
  return true;
}

void RealtimePlayer::Deinitialize() {
  if (synth_ == nullptr)
    return;
  ready_ = false;
  output_.Stop();
  synth_->Deinitialize();
  synth_->player_ = nullptr;
  synth_ = nullptr;
  state_ = desired_state_ = kStateStopped;
}

void RealtimePlayer::Play() {
  desired_state_ = kStatePlaying;
}

void RealtimePlayer::Pause() {
  if (desired_state_ == kStatePlaying)
    desired_state_ = kStatePaused;
}

void RealtimePlayer::Stop() {
  desired_state_ = kStateStopped;
  rewind_requested_ = true;
}

void RealtimePlayer::TogglePause() {
  if (desired_state_ == kStatePlaying)
    Pause();
  else
    Play();
}

double RealtimePlayer::GetPlaybackSeconds() const {
  auto rate = output_.sample_rate();
  return rate ? double(frames_played_.load()) / rate : 0.0;
}

void RealtimePlayer::RenderThunk(void* user, float* stereo_out, uint32_t frames) {
  static_cast<RealtimePlayer*>(user)->Render(stereo_out, frames);
}

void RealtimePlayer::Render(float* stereo_out, uint32_t frames) {
  // Never block the audio thread: if someone else is touching the synth, play silence.
  if (!ready_ || !synth_lock_.try_lock()) {
    memset(stereo_out, 0, sizeof(float) * frames * 2);
    return;
  }

  const int previous_state = state_.load();
  const int new_state = desired_state_.load();
  const bool rewind = rewind_requested_.exchange(false);
  // Leaving playback: release the file's notes, otherwise they'd hang in the idle render.
  if (rewind || (previous_state == kStatePlaying && new_state != kStatePlaying))
    synth_->AllNotesOff();
  if (rewind) {
    synth_->Reset();
    // With no song loaded, Reset() itself calls Stop() ("no more events"). Don't let that
    // queue another rewind, or we'd reset (and silence live notes) on every buffer.
    rewind_requested_ = false;
    frames_played_ = 0;
  }
  state_ = new_state;

  // Live input, applied at the start of the buffer (up to one period of timing jitter).
  if (midi_input_ != nullptr)
    midi_input_->Drain([this](uint32_t message) { DispatchMidi(message); });
  uint32_t message;
  while (ui_queue_.Pop(message))
    DispatchMidi(message);

  if (new_state == kStatePlaying) {
    synth_->RenderSamplesStereo(frames, stereo_out);
    frames_played_ += frames;
  } else {
    synth_->RenderIdleStereo(frames, stereo_out);
  }

  synth_lock_.unlock();

  Limit(stereo_out, frames);

  float peak = 0.0f;
  for (uint32_t i = 0; i < frames * 2; ++i) {
    float v = fabsf(stereo_out[i]);
    if (v > peak) peak = v;
  }
  float current = peak_level_.load(std::memory_order_relaxed);
  while (peak > current && !peak_level_.compare_exchange_weak(current, peak, std::memory_order_relaxed)) {
  }
}

// Stereo-linked peak limiter with instant attack: the envelope jumps to any new peak, so
// |output| never exceeds the threshold, then decays exponentially and the gain recovers
// smoothly. Replaces the old hard clip.
void RealtimePlayer::Limit(float* stereo, uint32_t frames) {
  float env = limiter_envelope_;
  float min_gain = 1.0f;
  for (uint32_t i = 0; i < frames; ++i) {
    float& l = stereo[i * 2];
    float& r = stereo[i * 2 + 1];
    float peak = fabsf(l) > fabsf(r) ? fabsf(l) : fabsf(r);
    env = peak > env ? peak : env * limiter_release_coef_;
    if (env > limiter_threshold_) {
      float gain = limiter_threshold_ / env;
      l *= gain;
      r *= gain;
      if (gain < min_gain) min_gain = gain;
    }
  }
  limiter_envelope_ = env;
  float current = limiter_min_gain_.load(std::memory_order_relaxed);
  while (min_gain < current && !limiter_min_gain_.compare_exchange_weak(current, min_gain, std::memory_order_relaxed)) {
  }
}

void RealtimePlayer::DispatchMidi(uint32_t message) {
  synth_->HandleMidiMessage(uint8_t(message & 0xFF), uint8_t((message >> 8) & 0x7F), uint8_t((message >> 16) & 0x7F));
}

}
}
}
