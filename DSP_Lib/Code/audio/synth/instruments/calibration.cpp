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
#include "calibration.h"

// File layout (little endian):
//   "DSPCAL01" or "DSPCAL02", u32 name length, name bytes, u32 note count, then per note:
//   i32 midi, f32 f0, f32 B, f32 b1, f32 b3, f32 peak, u32 rate, u32 n, f32[n] excitation
//   version 2 adds: f32 after_b1, f32 after_b3, f32 after_level, f32 detune_cents,
//                   u32 layer count, per layer: f32 velocity, f32 peak, u32 n, f32[n]

namespace dsp {
namespace audio {
namespace synth {
namespace instruments {

namespace {
const char kMagic1[8] = {'D', 'S', 'P', 'C', 'A', 'L', '0', '1'};
const char kMagic2[8] = {'D', 'S', 'P', 'C', 'A', 'L', '0', '2'};
}

bool StringCalibration::Save(const char* path) const {
  FILE* f = nullptr;
  if (fopen_s(&f, path, "wb") != 0 || !f) return false;
  auto u32 = [&](uint32_t v) { fwrite(&v, 4, 1, f); };
  auto f32 = [&](float v) { fwrite(&v, 4, 1, f); };
  auto floats = [&](const std::vector<float>& v) {
    u32(uint32_t(v.size()));
    fwrite(v.data(), sizeof(float), v.size(), f);
  };
  fwrite(kMagic2, 1, 8, f);
  u32(uint32_t(name.size()));
  fwrite(name.data(), 1, name.size(), f);
  u32(uint32_t(notes.size()));
  for (const CalibratedNote& n : notes) {
    u32(uint32_t(n.midi_note));
    f32(n.f0_hz); f32(n.inharmonicity); f32(n.b1); f32(n.b3); f32(n.peak);
    u32(n.excitation_rate);
    floats(n.excitation);
    f32(n.after_b1); f32(n.after_b3); f32(n.after_level); f32(n.detune_cents);
    u32(uint32_t(n.layers.size()));
    for (const ExcitationLayer& l : n.layers) {
      f32(l.velocity); f32(l.peak);
      floats(l.samples);
    }
  }
  bool ok = ferror(f) == 0;
  fclose(f);
  return ok;
}

bool StringCalibration::Load(const char* path) {
  FILE* f = nullptr;
  if (fopen_s(&f, path, "rb") != 0 || !f) return false;
  bool ok = true;
  auto read = [&](void* p, size_t bytes) { if (ok && fread(p, 1, bytes, f) != bytes) ok = false; };
  auto read_floats = [&](std::vector<float>& v) {
    uint32_t count = 0;
    read(&count, 4);
    if (!ok || count > 10000000) { ok = false; return; }
    v.resize(count);
    read(v.data(), count * sizeof(float));
  };
  char magic[8];
  read(magic, 8);
  int version = 0;
  if (ok && !memcmp(magic, kMagic1, 8)) version = 1;
  if (ok && !memcmp(magic, kMagic2, 8)) version = 2;
  ok = ok && version > 0;
  uint32_t len = 0, count = 0;
  read(&len, 4);
  if (ok && len < 4096) { name.resize(len); read(&name[0], len); } else ok = false;
  read(&count, 4);
  if (ok && count > 1000) ok = false;
  notes.clear();
  for (uint32_t i = 0; ok && i < count; ++i) {
    CalibratedNote n;
    uint32_t midi = 0;
    read(&midi, 4);
    n.midi_note = int(midi);
    read(&n.f0_hz, 4); read(&n.inharmonicity, 4); read(&n.b1, 4); read(&n.b3, 4); read(&n.peak, 4);
    read(&n.excitation_rate, 4);
    read_floats(n.excitation);
    if (version >= 2) {
      read(&n.after_b1, 4); read(&n.after_b3, 4); read(&n.after_level, 4); read(&n.detune_cents, 4);
      uint32_t layers = 0;
      read(&layers, 4);
      if (!ok || layers > 64) { ok = false; break; }
      n.layers.resize(layers);
      for (ExcitationLayer& l : n.layers) {
        read(&l.velocity, 4); read(&l.peak, 4);
        read_floats(l.samples);
      }
    }
    notes.push_back(std::move(n));
  }
  fclose(f);
  if (ok)
    std::sort(notes.begin(), notes.end(),
              [](const CalibratedNote& a, const CalibratedNote& b) { return a.midi_note < b.midi_note; });
  return ok;
}

}
}
}
}
