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

#include "base.h"
#include "synth.h"

namespace dsp {
namespace audio {
namespace synth {

// What a synth needs from whatever is driving it (Player, RealtimePlayer, ...).
// Stop() may be called from the audio thread (e.g. at the end of a song).
class SynthHost {
 public:
  virtual ~SynthHost() {}
  virtual void Stop() = 0;
  double song_length_ms = 0;
};

class Synth : public Component {
 public:
  SynthHost* player_;
  Synth() : Component(),player_(nullptr) {

  }
  virtual ~Synth() {

  }
  virtual void Initialize() = 0;
  virtual void Deinitialize() = 0;
  virtual void Reset() = 0;
  // Renders the next block, advancing any sequencer (file playback).
  virtual void RenderSamplesStereo(uint32_t samples_count, real_t* data_out) = 0;

  // Live input support. Called on the audio thread only.
  // Renders without advancing the sequencer, so live notes and release tails keep sounding
  // while file playback is stopped or paused.
  virtual void RenderIdleStereo(uint32_t samples_count, real_t* data_out) {
    memset(data_out, 0, sizeof(real_t) * samples_count * 2);
  }
  // A raw MIDI channel message (status byte + up to two data bytes).
  virtual void HandleMidiMessage(uint8_t status, uint8_t data1, uint8_t data2) {}
  // Release every sounding note on every channel.
  virtual void AllNotesOff() {}
  // Move file playback to a position (ms). Only call with the audio thread kept out.
  virtual void Seek(double ms) {}
  // Current song position (ms), or < 0 if the synth does not track it.
  virtual double PlaybackMs() const { return -1.0; }
 protected:
};

}
}
}