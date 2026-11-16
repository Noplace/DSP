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
#include <algorithm>
#include <stdio.h>
#include <string.h>
#include "drum_calibration.h"

// File layout (little endian):
//   "DSPDRM01", u32 name length, name, u32 note count, then per note:
//   i32 midi, i32 from_note, f32 semitones, f32 gain, u32 rate, u32 take count, per take:
//     f32 velocity, f32 level, u32 source length, source, u32 mode count,
//     per mode f32 freq, f32 t60, f32 re, f32 im, then f32 residual_scale, u32 n, i16[n] residual

namespace dsp {
namespace audio {
namespace synth {
namespace instruments {

namespace {
const char kMagic[8] = {'D', 'S', 'P', 'D', 'R', 'M', '0', '1'};
}

bool DrumCalibration::Save(const char* file) const {
  FILE* f = nullptr;
  if (fopen_s(&f, file, "wb") != 0 || !f) return false;
  auto u32 = [&](uint32_t v) { fwrite(&v, 4, 1, f); };
  auto f32 = [&](float v) { fwrite(&v, 4, 1, f); };
  auto str = [&](const std::string& s) { u32(uint32_t(s.size())); fwrite(s.data(), 1, s.size(), f); };
  fwrite(kMagic, 1, 8, f);
  str(name);
  u32(uint32_t(notes.size()));
  for (const DrumCalNote& n : notes) {
    u32(uint32_t(n.midi_note)); u32(uint32_t(n.from_note)); f32(n.semitones); f32(n.gain); u32(n.rate);
    u32(uint32_t(n.takes.size()));
    for (const DrumCalTake& t : n.takes) {
      f32(t.velocity); f32(t.level);
      str(t.source);
      u32(uint32_t(t.modes.size()));
      for (const DrumCalMode& m : t.modes) { f32(m.freq_hz); f32(m.t60_s); f32(m.re); f32(m.im); }
      f32(t.residual_scale);
      u32(uint32_t(t.residual.size()));
      fwrite(t.residual.data(), sizeof(int16_t), t.residual.size(), f);
    }
  }
  bool ok = ferror(f) == 0;
  fclose(f);
  return ok;
}

bool DrumCalibration::Load(const char* file) {
  FILE* f = nullptr;
  if (fopen_s(&f, file, "rb") != 0 || !f) return false;
  bool ok = true;
  auto read = [&](void* p, size_t bytes) { if (ok && fread(p, 1, bytes, f) != bytes) ok = false; };
  auto u32 = [&]() { uint32_t v = 0; read(&v, 4); return v; };
  auto f32 = [&]() { float v = 0; read(&v, 4); return v; };
  auto str = [&](std::string& s) {
    uint32_t len = u32();
    if (!ok || len > 4096) { ok = false; return; }
    s.resize(len);
    if (len) read(&s[0], len);
  };
  char magic[8];
  read(magic, 8);
  ok = ok && !memcmp(magic, kMagic, 8);
  str(name);
  const uint32_t count = u32();
  if (count > 256) ok = false;
  notes.clear();
  for (uint32_t i = 0; ok && i < count; ++i) {
    DrumCalNote n;
    n.midi_note = int(u32()); n.from_note = int(u32()); n.semitones = f32(); n.gain = f32(); n.rate = u32();
    const uint32_t takes = u32();
    if (takes > 256) { ok = false; break; }
    n.takes.resize(takes);
    for (DrumCalTake& t : n.takes) {
      t.velocity = f32(); t.level = f32();
      str(t.source);
      const uint32_t modes = u32();
      if (!ok || modes > 1024) { ok = false; break; }
      t.modes.resize(modes);
      for (DrumCalMode& m : t.modes) { m.freq_hz = f32(); m.t60_s = f32(); m.re = f32(); m.im = f32(); }
      t.residual_scale = f32();
      const uint32_t samples = u32();
      if (!ok || samples > 48000 * 20) { ok = false; break; }
      t.residual.resize(samples);
      read(t.residual.data(), samples * sizeof(int16_t));
    }
    notes.push_back(std::move(n));
  }
  fclose(f);
  if (ok) path = file;
  return ok;
}

const DrumCalNote* DrumCalibration::Find(int midi_note) const {
  for (const DrumCalNote& n : notes)
    if (n.midi_note == midi_note) return &n;
  return nullptr;
}

const DrumCalNote* DrumCalibration::Source(int midi_note, float* semitones, float* gain) const {
  const DrumCalNote* n = Find(midi_note);
  *semitones = 0;
  *gain = 1;
  for (int hops = 0; n && n->from_note >= 0 && hops < 4; ++hops) {  // derived from another note
    *semitones += n->semitones;
    *gain *= n->gain;
    n = Find(n->from_note);
  }
  return n && !n->takes.empty() ? n : nullptr;
}

}
}
}
}
