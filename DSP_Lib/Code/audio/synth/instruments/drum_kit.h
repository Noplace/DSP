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

namespace dsp {
namespace audio {
namespace synth {
namespace instruments {

// Physically modelled GM drum kit (MIDI channel 10, notes 27-87).
//
// The kit is a set of physical objects ("pieces": kick, snare, toms, hi-hat, cymbals, hand
// percussion). Each GM note is an articulation of one piece: 42/44/46 are the closed, pedal and
// open strokes of the same hi-hat, 51/53 bow and bell of the same ride, 37/38/40 side stick,
// stroke and rim shot on the same snare. A new hit adds to the piece's ringing vibration, as it
// does on a real drum, so a closed hi-hat chokes the open one by damping its modes, a roll builds
// up resonance and a cymbal re-struck while it rings doesn't double up.
//
//  - Membranes: modes of a circular membrane (Bessel zeros) with air loading and shell-cavity
//    stiffness, excited by a contact-force pulse whose length shortens with velocity (a harder
//    hit is brighter), at a strike position that varies hit to hit. Tension modulation raises
//    the pitch with the head's vibration energy, so loud hits start sharp and glide down.
//    Two-headed drums add the resonant head.
//  - Snare wires: noise gated by the snare-side head's displacement through a one-sided contact
//    (the wires only rattle while the head moves away from them faster than they can follow).
//  - Cymbals and hi-hat: clouds of 100-250 inharmonic modes driven by the stick pulse plus a
//    noise-driven energy cascade from low to high modes (the nonlinear "bloom" of a crash).
//  - Shakers, tambourine, guiro, vibraslap: stochastic collisions driving resonators (PhISEM).
//
// Modes are complex phasors, four at a time with SSE, rendered in 32-sample blocks.
class DrumKit : public InstrumentProcessor {
 public:
  DrumKit();
  ~DrumKit();

  int Load() override;
  int Unload() override;
  InstrumentData* NewInstrumentData() override { return new InstrumentData(); }
  real_t Tick(int v) override;
  int SetFrequency(real_t freq, int v) override;
  int NoteOn(int v) override;
  int NoteOff(int v) override;
  void set_instrument_data(InstrumentData* idata) override { cdata_ = idata; }
  bool IsVoiceActive(int v) const override;
  real_t VoiceLevel(int v) const override;
  bool HandlesVelocity() const override { return true; }
  void BendFrequency(real_t, int) override {}
  real_t VoicePan(int v) const override;

  // Name of a GM drum note (for tools), or nullptr if unmapped.
  static const char* DrumName(int note);
  // Name of the physical piece a note plays and its number of modes (for tools/CPU reports).
  static const char* PieceName(int note);
  int PieceModes(int note) const;

  struct Impl;

 private:
  Impl* impl_;
  InstrumentData* cdata_;
};

}
}
}
}
