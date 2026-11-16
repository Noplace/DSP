/*****************************************************************************************************************
* Copyright (c) 2012 Khalid Ali Al-Kooheji                                                                       *
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

#include <memory>
#include <string>
#include "../base.h"
#include "../adsr.h"

namespace dsp {
namespace audio {
namespace synth {
namespace instruments {

class InstrumentData {
 public:
  NoteData note_data_array[Polyphony];
  InstrumentData() {
    memset(note_data_array,0,sizeof(note_data_array));
  }
  virtual ~InstrumentData() {

  }
};

// An instrument owns a pool of Polyphony voices (index 0..Polyphony-1). All per-voice state
// (envelope, oscillator phase, string buffers...) lives in the instrument, and channels borrow
// voices from the pool, so several channels can play the same instrument without sharing a
// voice. Each allocation bumps the voice's generation; a channel holding an older generation
// knows its voice was stolen.
class InstrumentProcessor : public Component {
 public:
  InstrumentProcessor() : Component() , loaded_(false), age_counter_(0) {
    memset(voice_gen_, 0, sizeof(voice_gen_));
    memset(voice_age_, 0, sizeof(voice_age_));
  }
  virtual ~InstrumentProcessor() {

  }
  virtual int Load() = 0;
  virtual int Unload() = 0;
  virtual InstrumentData* NewInstrumentData() = 0;
  virtual real_t Tick(int note_index) = 0;
  virtual int SetFrequency(real_t freq, int note_index) = 0;
  virtual int NoteOn(int note_index) = 0;
  virtual int NoteOff(int note_index) = 0;
  virtual void set_instrument_data(InstrumentData* idata) = 0;

  // Still producing sound (attack/decay/sustain/release)?
  virtual bool IsVoiceActive(int v) const { return adsr[v].stage() != 0; }
  // Rough current loudness of a voice (used to pick which releasing voice to steal).
  virtual real_t VoiceLevel(int v) const { return adsr[v].value(); }
  // True if the instrument turns velocity into loudness itself (e.g. recorded velocity layers),
  // so the channel must not apply its generic velocity curve on top.
  virtual bool HandlesVelocity() const { return false; }
  // Stereo position of a voice inside the channel (0 left .. 1 right), or < 0 to leave it in the
  // middle. A drum kit places each piece where it sits in the kit.
  virtual real_t VoicePan(int v) const { (void)v; return -1; }
  // Mixer group a voice plays into (DrumGroup, drum_groups.h), or -1 for the channel itself.
  virtual int VoiceGroup(int v) const { (void)v; return -1; }
  // Which calibration file (measured from recordings) this instrument plays, with a short
  // summary, or empty for a hand-set preset. Shown in the studio's instrument inspector.
  virtual std::string CalibrationInfo() const { return std::string(); }
  // Pitch change on a sounding voice (pitch bend). Instruments that can't glide may ignore it.
  virtual void BendFrequency(real_t freq, int v) { SetFrequency(freq, v); }

  // Load and bind the voice state. Call once, off the audio thread.
  void Prepare() {
    Load();
    if (!data_) {
      data_.reset(NewInstrumentData());
      set_instrument_data(data_.get());
    }
  }

  // Pick a voice: a silent one if possible, else the quietest releasing voice, else the oldest.
  int AllocateVoice(uint32_t& generation) {
    int best = -1;
    for (int v = 0; v < Polyphony && best < 0; ++v) {
      if (!data_->note_data_array[v].on && !IsVoiceActive(v))
        best = v;
    }
    if (best < 0) {
      real_t lowest = 1e9f;
      for (int v = 0; v < Polyphony; ++v) {
        if (!data_->note_data_array[v].on && VoiceLevel(v) < lowest) {
          lowest = VoiceLevel(v);
          best = v;
        }
      }
    }
    if (best < 0) {
      uint32_t oldest = 0xFFFFFFFF;
      for (int v = 0; v < Polyphony; ++v) {
        if (voice_age_[v] < oldest) {
          oldest = voice_age_[v];
          best = v;
        }
      }
    }
    voice_age_[best] = ++age_counter_;
    generation = ++voice_gen_[best];
    return best;
  }
  void StartVoice(int v, int note, real_t freq, real_t velocity) {
    auto& nd = data_->note_data_array[v];
    nd.note = note;
    nd.velocity = velocity;
    nd.on = true;
    nd.quiet_release = false;
    SetFrequency(freq, v);
    NoteOn(v);
  }
  // quiet: the key did not come up just now (sustain pedal lifted, re-strike, all notes off), so
  // instruments with a key-up sound (piano dampers) leave it out.
  void ReleaseVoice(int v, bool quiet = false) {
    auto& nd = data_->note_data_array[v];
    if (!nd.on)
      return;
    nd.on = false;
    nd.quiet_release = quiet;
    NoteOff(v);
  }
  uint32_t voice_generation(int v) const { return voice_gen_[v]; }

 protected:
  ADSR adsr[Polyphony];
  bool loaded_;

 private:
  std::unique_ptr<InstrumentData> data_;
  uint32_t voice_gen_[Polyphony];
  uint32_t voice_age_[Polyphony];
  uint32_t age_counter_;
};

}
}
}
}
