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



namespace dsp {
namespace audio {
namespace synth {

class MidiSynth;

// One MIDI channel: current program, controllers and the voices it is sounding.
//
// Voices are borrowed from the instrument that started them, so a note keeps sounding with
// its own instrument after a program change, and channels sharing an instrument never share a
// voice. Only sounding voices are rendered.
class Channel : public Component {
 public:
  static const int kMaxVoices = 64;

  Channel(int index) : Component(),index(index),instrument_(nullptr),buffer_(nullptr),mono_(nullptr),
                       mono_capacity_(0),voice_count_(0),amplification_(1.0f),expression_(1.0f),
                       pitch_bend_range_(2.0f),bend_factor_(1.0f),aux_send(0),param_fine(127),
                       param_coarse(127),des_fine(0),des_coarse(2),silence_(false),sustain_(false) {
    set_panning(0.5);
    memset(&effects,0,sizeof(effects));
  }
  ~Channel() {
  }

  void RenderStereo(uint32_t samples_to_generate) {
    // Drop voices that finished their release or were stolen by another channel.
    for (int i = 0; i < voice_count_;) {
      auto& v = voices_[i];
      bool stolen = v.instrument->voice_generation(v.index) != v.generation;
      bool finished = !v.held && !v.sustained && !v.instrument->IsVoiceActive(v.index);
      if (stolen || finished)
        voices_[i] = voices_[--voice_count_];
      else
        ++i;
    }

    if (voice_count_ == 0 || mono_ == nullptr) {
      memset(buffer_, 0, sizeof(real_t) * samples_to_generate * 2);
      return;
    }

    memset(mono_, 0, sizeof(real_t) * samples_to_generate);
    for (int i = 0; i < voice_count_; ++i) {
      auto& v = voices_[i];
      auto inst = v.instrument;
      const int index = v.index;
      const real_t gain = v.gain;
      for (uint32_t s = 0; s < samples_to_generate; ++s)
        mono_[s] += inst->Tick(index) * gain;
    }

    const real_t level = amplification_ * expression_;
    const real_t left = level * pan_l, right = level * pan_r;
    auto bufptr = buffer_;
    for (uint32_t s = 0; s < samples_to_generate; ++s) {
      *bufptr++ = left * mono_[s];
      *bufptr++ = right * mono_[s];
    }
    //effects processing
    for (int i=0;i<kEffectCount;++i) {
      if (effects[i] != nullptr)
        effects[i]->ProcessStereo(buffer_,buffer_,samples_to_generate);
    }
  }

  void AddNote(int note,real_t freq,real_t velocity) {
    if (instrument_ == nullptr)
      return;
    // Re-striking a key that's still sounding: release the old voice first.
    for (int i = 0; i < voice_count_; ++i) {
      auto& v = voices_[i];
      if (v.note == note && (v.held || v.sustained)) {
        v.held = v.sustained = false;
        v.instrument->ReleaseVoice(v.index, true);  // the new strike, not a key-up
      }
    }
    if (voice_count_ == kMaxVoices) {
      // Channel full: let go of the oldest entry.
      voices_[0].instrument->ReleaseVoice(voices_[0].index, true);
      voices_[0] = voices_[--voice_count_];
    }
    Voice v;
    v.instrument = instrument_;
    v.index = instrument_->AllocateVoice(v.generation);
    v.note = note;
    v.base_freq = freq;
    // GM-style velocity curve (40*log10(v) dB), unless the instrument does velocity itself.
    v.gain = instrument_->HandlesVelocity() ? 1.0f : velocity * velocity;
    v.held = true;
    v.sustained = false;
    instrument_->StartVoice(v.index, note, freq * bend_factor_, velocity);
    voices_[voice_count_++] = v;
  }

  void RemoveNote(int note,real_t velocity) {
    for (int i = 0; i < voice_count_; ++i) {
      auto& v = voices_[i];
      if (v.note != note || !v.held)
        continue;
      v.held = false;
      if (sustain_)
        v.sustained = true;  // keep sounding until the pedal comes up
      else
        v.instrument->ReleaseVoice(v.index);
    }
  }

  void AllNotesOff() {
    for (int i = 0; i < voice_count_; ++i) {
      auto& v = voices_[i];
      if (v.held || v.sustained) {
        v.held = v.sustained = false;
        v.instrument->ReleaseVoice(v.index, true);  // stop/panic, not a performed key-up
      }
    }
  }

  void set_sustain(bool on) {
    sustain_ = on;
    if (!on) {
      for (int i = 0; i < voice_count_; ++i) {
        auto& v = voices_[i];
        if (v.sustained) {
          v.sustained = false;
          v.instrument->ReleaseVoice(v.index, true);  // pedal up: the key came up earlier
        }
      }
    }
  }
  void set_expression(real_t expression) { expression_ = expression; }
  void ResetControllers() {
    set_sustain(false);
    expression_ = 1.0f;
    SetPitchBend(0);
  }

  // amount in semitones
  void SetPitchBend(real_t amount) {
    bend_factor_ = exp(amount * _LN_2_DIV_12);
    for (int i = 0; i < voice_count_; ++i) {
      auto& v = voices_[i];
      if (v.instrument->voice_generation(v.index) == v.generation)
        v.instrument->BendFrequency(v.base_freq * bend_factor_, v.index);
    }
  }

  instruments::InstrumentProcessor* instrument() { return instrument_; }
  // New notes use this instrument; sounding notes keep theirs.
  void set_instrument(instruments::InstrumentProcessor* instrument) {
    instrument_ = instrument;
  }
  void set_amplification(real_t amplification) {
    amplification_ = amplification;
  }
  void set_panning(real_t panning) {
    panning_ = panning;
    pan_l = sqrt(1.0f-panning_);
    pan_r = sqrt(panning_);
  }
  bool silence() { return silence_;  }
  void set_silence(bool silence) { silence_ = silence; }
  real_t pitch_bend_range() {
    return pitch_bend_range_;
  }
  void set_pitch_bend_range(real_t pitch_bend_range) {
    pitch_bend_range_ = pitch_bend_range;
  }
  int active_voices() const { return voice_count_; }
  real_t* buffer() {
    return buffer_;
  }
  void set_buffer(real_t* buffer) { buffer_ = buffer; }
  // Scratch buffer for summing voices, at least as many frames as one render block.
  void set_mono_buffer(real_t* mono, uint32_t capacity) { mono_ = mono; mono_capacity_ = capacity; }

 private:
  friend MidiSynth;
  struct Voice {
    instruments::InstrumentProcessor* instrument;
    int index;
    uint32_t generation;
    int note;
    real_t base_freq;  // unbent frequency
    real_t gain;       // from velocity
    bool held;         // key is down
    bool sustained;    // key released while the sustain pedal was down
  };
  effects::Effect* effects[kEffectCount];
  instruments::InstrumentProcessor* instrument_;
  real_t* buffer_;
  real_t* mono_;
  uint32_t mono_capacity_;
  Voice voices_[kMaxVoices];
  int voice_count_;
  real_t amplification_,expression_,panning_,pan_l,pan_r,pitch_bend_range_,bend_factor_,aux_send;
  int index;
  uint8_t param_fine,param_coarse,des_fine,des_coarse;  // RPN select + data entry
  bool silence_;
  bool sustain_;
};

}
}
}
