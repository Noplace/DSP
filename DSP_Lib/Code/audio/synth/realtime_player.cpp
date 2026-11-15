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
    : synth_(nullptr), midi_input_(nullptr), peak_level_(0.0f), limiter_min_gain_(1.0f),
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
  limiter_.Configure(output_.sample_rate());
  loudness_.Configure(output_.sample_rate());
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

void RealtimePlayer::Seek(double seconds) {
  if (!ready_)
    return;
  if (seconds < 0)
    seconds = 0;
  WithSynthLocked([&] {
    rewind_requested_ = false;  // a pending rewind would undo the seek
    synth_->Seek(seconds * 1000.0);
    frames_played_ = uint64_t(seconds * output_.sample_rate());
  });
  // Seeking while stopped arms the position for the next Play().
}

double RealtimePlayer::GetPlaybackSeconds() const {
  if (synth_ != nullptr) {  // synths that track the song position (loops, speed) know best
    const double ms = synth_->PlaybackMs();
    if (ms >= 0) return ms / 1000.0;
  }
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

  // Live input, applied at the start of the buffer (up to one period of timing jitter). While
  // recording, each message is also passed to the UI with the song position it arrived at.
  const bool record = recording_ && new_state == kStatePlaying;
  const double song_ms = record ? synth_->PlaybackMs() : 0.0;
  auto live = [&](uint32_t message) {
    if (record) record_queue_.Push({message, song_ms});
    DispatchMidi(message);
  };
  if (midi_input_ != nullptr)
    midi_input_->Drain(live);
  uint32_t message;
  while (ui_queue_.Pop(message))
    live(message);

  if (new_state == kStatePlaying) {
    synth_->RenderSamplesStereo(frames, stereo_out);
    frames_played_ += frames;
  } else {
    synth_->RenderIdleStereo(frames, stereo_out);
  }

  synth_lock_.unlock();

  Limit(stereo_out, frames);
  loudness_.Process(stereo_out, frames);

  float peak = 0.0f;
  for (uint32_t i = 0; i < frames * 2; ++i) {
    float v = fabsf(stereo_out[i]);
    if (v > peak) peak = v;
  }
  float current = peak_level_.load(std::memory_order_relaxed);
  while (peak > current && !peak_level_.compare_exchange_weak(current, peak, std::memory_order_relaxed)) {
  }
}

void RealtimePlayer::Limit(float* stereo, uint32_t frames) {
  float min_gain = limiter_.Process(stereo, frames);
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
