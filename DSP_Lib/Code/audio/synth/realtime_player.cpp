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
#include "../output/wasapi_output.h"
#include "realtime_player.h"

namespace dsp {
namespace audio {
namespace synth {

RealtimePlayer::RealtimePlayer()
    : synth_(nullptr), desired_state_(kStateStopped), state_(kStateStopped), rewind_requested_(false),
      ready_(false), frames_played_(0) {
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

  if (rewind_requested_.exchange(false)) {
    synth_->Reset();
    frames_played_ = 0;
  }
  state_ = desired_state_.load();

  if (state_ == kStatePlaying) {
    synth_->RenderSamplesStereo(frames, stereo_out);
    frames_played_ += frames;
  } else {
    memset(stereo_out, 0, sizeof(float) * frames * 2);
  }

  synth_lock_.unlock();
}

}
}
}
