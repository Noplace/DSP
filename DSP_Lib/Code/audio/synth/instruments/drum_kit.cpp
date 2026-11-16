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
#define NOMINMAX  // std::min/std::max, not the windows.h macros
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <vector>
#include <emmintrin.h>
#include "../../../dsp.h"

namespace dsp {
namespace audio {
namespace synth {
namespace instruments {

namespace {

const double kPi = 3.14159265358979323846;
const float kLn1000 = 6.907755f;  // 60 dB
const int kBlock = 32;            // samples rendered per piece at a time
const int kFirstNote = 27, kLastNote = 87, kNoteCount = kLastNote - kFirstNote + 1;
const int kPositions = 17;        // membrane strike-position table: radius 0, 1/16, ..., 1
const int kMaxMembraneModes = 72;
const float kKitGain = 0.3f;      // whole kit ~10 dB down so a full mix leaves headroom

// ============================================================================================
// Definitions: what each physical piece is, and how each GM note plays it.
// ============================================================================================

enum BodyType { kNoBody, kMembraneBody, kMetalBody, kModeListBody };

// Circular membrane (one or two heads on a shell).
struct MembraneDef {
  float f01;         // tuned fundamental, Hz
  float air;         // air loading rho*a/sigma: the air mass lowers the low modes most
  float cavity;      // shell air stiffness on the axisymmetric (0,n) modes
  float max_ratio;   // modes up to this multiple of f01
  float t60;         // decay of the fundamental region, s
  float t60_axisym;  // (0,n) modes radiate strongly (monopoles) and so decay faster
  float damp_hz;     // T60(f) = t60 / (1 + (f - f01) / damp_hz)
  float tilt;        // higher modes quieter: level ~ (f01 / f)^tilt
  float split;       // degenerate (m>0) pairs split by this fraction: slow beating, 0 = none
  float glide;       // tension modulation: pitch fraction above rest at full energy
  float reso_ratio;  // resonant head fundamental / f01 (0 = single-headed)
  float reso_gain, reso_t60;
};

// Cymbal / hi-hat: a cloud of inharmonic modes (plate modes are dense and irregular).
struct MetalDef {
  float f_lo, f_hi;
  int count;
  uint32_t seed;            // the same cymbal always has the same modes
  float peak_lo, peak_hi;   // spectral envelope: rises to peak_lo, falls above peak_hi
  float spread;             // log-normal scatter of mode levels
  float t60_lo, t60_hi;     // decay at f_lo and at f_hi (log-interpolated)
  float cascade;            // noise-driven energy cascade into the upper modes (the "bloom")
  float cascade_hz;         // modes above this receive it
  float cascade_attack_ms, cascade_t60;
  float bell_hz, bell_t60;  // ride bell partials (bell_count of them)
  int bell_count;
};

struct ModeListDef {
  int count;
  Mode modes[12];  // Hz, gain, T60 s
};

// Filtered noise straight to the output (hand claps, breath, scrape noise...).
struct NoiseDef { float amount, hz, q, hp_hz, t60, tail, tail_t60, sweep, sweep_ms; };
// Snare wires: noise gated by the snare-side head displacement.
struct WireDef { float amount, threshold, hp_hz, lp_hz, ring_ms; };
// A driven tone (whistle, cuica friction drum, synth "High Q").
struct ToneDef { float amount, hz, h2, h3, glide, glide_ms, warble_hz, warble_depth, attack_ms, hold_ms, t60; };
// Stochastic / periodic collisions (shakers, jingles, guiro scrape, vibraslap, clap bursts).
struct RattleDef {
  float amount, rate_hz;
  float rate_exp;    // collision rate ~ energy^rate_exp (shakers: 1)
  float randomness;  // 0 = periodic, 1 = Poisson
  float t60;         // shaking energy decay
  int max_events;    // 0 = unlimited
  bool to_noise;     // events retrigger the noise layer (claps) instead of driving the modes
  bool sequential;   // event i rings only mode i (bell tree)
};

struct PieceDef {
  const char* name;
  BodyType body;
  MembraneDef mem;
  MetalDef metal;
  ModeListDef list;
  NoiseDef noise;
  WireDef wires;
  ToneDef tone;
  RattleDef rattle;
  float pan;   // 0 left .. 1 right, drummer's perspective
  float gain;
};

struct Articulation {
  const char* name;
  int piece;           // -1 = unmapped
  float pos, pos_jitter;  // membrane: strike radius 0 centre .. 1 rim; metal: 0 edge/bow .. 1 bell
  float contact_ms;    // contact time at full velocity: stick ~0.3-0.6, hand ~1.5, felt beater ~3.5
  float contact_exp;   // contact ~ velocity^-exp (Hertzian contact ~0.2; felt stiffens: more)
  float drive;         // how much of the hit reaches the body
  float click, click_hz, click_q, click_ms;  // contact noise (stick/beater/rim), T60 in ms
  float noise, wires, tone, rattle, cascade;  // scale the piece's layers
  float damp;          // body T60 multiplier from this stroke on (1 free; closed hi-hat ~0.05)
  float length;        // scales tone hold / rattle duration
  float pitch;         // scales tone / noise frequency
  float gain;
};

Articulation Stroke(const char* name, int piece) {
  Articulation a = {};
  a.name = name;
  a.piece = piece;
  a.pos = 0.3f; a.pos_jitter = 0.05f;
  a.contact_ms = 0.5f; a.contact_exp = 0.2f;
  a.drive = 1;
  a.click_hz = 4000; a.click_q = 0.7f; a.click_ms = 4;
  a.noise = a.wires = a.tone = a.rattle = a.cascade = 1;
  a.damp = a.length = a.pitch = a.gain = 1;
  return a;
}
Articulation& Click(Articulation& a, float amount, float hz, float q, float ms) {
  a.click = amount; a.click_hz = hz; a.click_q = q; a.click_ms = ms;
  return a;
}

PieceDef MakePiece(const char* name, BodyType body, float pan, float gain) {
  PieceDef p = {};
  p.name = name; p.body = body; p.pan = pan; p.gain = gain;
  return p;
}

PieceDef Drum(const char* name, float pan, float gain, float f01, float air, float cavity, float max_ratio, float t60,
              float t60_axisym, float damp_hz, float tilt, float split, float glide) {
  PieceDef p = MakePiece(name, kMembraneBody, pan, gain);
  p.mem = {f01, air, cavity, max_ratio, t60, t60_axisym, damp_hz, tilt, split, glide, 0, 0, 0};
  return p;
}
PieceDef& Reso(PieceDef& p, float ratio, float gain, float t60) {
  p.mem.reso_ratio = ratio; p.mem.reso_gain = gain; p.mem.reso_t60 = t60;
  return p;
}

PieceDef Cymbal(const char* name, float pan, float gain, float f_lo, float f_hi, int count, uint32_t seed, float peak_lo,
                float peak_hi, float spread, float t60_lo, float t60_hi, float cascade, float cascade_hz,
                float cascade_attack_ms, float cascade_t60) {
  PieceDef p = MakePiece(name, kMetalBody, pan, gain);
  p.metal = {f_lo, f_hi, count, seed, peak_lo, peak_hi, spread, t60_lo, t60_hi, cascade, cascade_hz,
             cascade_attack_ms, cascade_t60, 0, 0, 0};
  return p;
}

PieceDef Modes(const char* name, float pan, float gain, std::initializer_list<Mode> modes) {
  PieceDef p = MakePiece(name, kModeListBody, pan, gain);
  for (const Mode& m : modes)
    if (p.list.count < 12) p.list.modes[p.list.count++] = m;
  return p;
}
PieceDef& Noise(PieceDef& p, float amount, float hz, float q, float t60, float hp_hz = 0) {
  p.noise.amount = amount; p.noise.hz = hz; p.noise.q = q; p.noise.t60 = t60; p.noise.hp_hz = hp_hz;
  return p;
}
PieceDef& Rattle(PieceDef& p, float amount, float rate_hz, float rate_exp, float randomness, float t60, int max_events = 0) {
  p.rattle.amount = amount; p.rattle.rate_hz = rate_hz; p.rattle.rate_exp = rate_exp;
  p.rattle.randomness = randomness; p.rattle.t60 = t60; p.rattle.max_events = max_events;
  return p;
}
PieceDef& Tone(PieceDef& p, float amount, float hz, float h2, float h3, float glide, float glide_ms, float warble_hz,
               float warble_depth, float attack_ms, float hold_ms, float t60) {
  p.tone = {amount, hz, h2, h3, glide, glide_ms, warble_hz, warble_depth, attack_ms, hold_ms, t60};
  return p;
}

struct KitTable {
  std::vector<PieceDef> pieces;
  Articulation notes[kNoteCount];

  int Add(const PieceDef& p) {
    pieces.push_back(p);
    return int(pieces.size()) - 1;
  }
  Articulation& Set(int note, const Articulation& a) {
    notes[note - kFirstNote] = a;
    return notes[note - kFirstNote];
  }

  KitTable() {
    for (auto& n : notes) n = Stroke(nullptr, -1);

    // ---- Kicks: 22" and 24" two-headed bass drums, felt beater, lightly damped -------------
    {
      PieceDef kick = Drum("Kick 22\"", 0.5f, 1.15f, 57, 0.95f, 0.25f, 6.0f, 0.42f, 1.0f, 110, 0.55f, 0.004f, 0.35f);
      Reso(kick, 1.07f, 0.45f, 0.32f);
      int k1 = Add(kick);
      Articulation& a = Set(36, Stroke("Bass Drum 1", k1));
      a.pos = 0.12f; a.pos_jitter = 0.04f; a.contact_ms = 3.2f; a.contact_exp = 0.35f;
      Click(a, 1.0f, 3300, 0.8f, 7);

      PieceDef kick2 = Drum("Kick 24\"", 0.5f, 1.15f, 49, 1.05f, 0.25f, 6.0f, 0.7f, 1.0f, 90, 0.5f, 0.004f, 0.3f);
      Reso(kick2, 1.05f, 0.55f, 0.5f);
      int k2 = Add(kick2);
      Articulation& b = Set(35, Stroke("Acoustic Bass Drum", k2));
      b.pos = 0.15f; b.contact_ms = 4.0f; b.contact_exp = 0.35f;
      Click(b, 0.35f, 2600, 0.8f, 8);
    }

    // ---- Snare: 14" x 5.5", coated batter, thin snare-side head with wires -------------------
    {
      PieceDef snare = Drum("Snare 14\"", 0.48f, 0.95f, 192, 0.5f, 0.2f, 9.0f, 0.5f, 0.6f, 380, 0.3f, 0.003f, 0.05f);
      Reso(snare, 1.62f, 0.5f, 0.35f);
      snare.wires = {9.0f, 0.004f, 900, 10000, 12};
      int s = Add(snare);
      Articulation& hit = Set(38, Stroke("Acoustic Snare", s));
      hit.pos = 0.35f; hit.pos_jitter = 0.07f; hit.contact_ms = 0.5f;
      Click(hit, 0.4f, 4600, 0.7f, 5);
      Articulation& rim = Set(40, Stroke("Electric Snare", s));  // played as a rim shot
      rim.pos = 0.88f; rim.pos_jitter = 0.03f; rim.contact_ms = 0.3f; rim.gain = 1.6f;
      Click(rim, 1.5f, 2300, 1.3f, 14);
      Articulation& side = Set(37, Stroke("Side Stick", s));     // cross stick: wood on rim
      side.pos = 0.85f; side.drive = 0.12f; side.contact_ms = 0.4f; side.wires = 0.3f;
      Click(side, 1.6f, 1750, 5.0f, 45);
    }

    // ---- Hand clap: a few bursts a few ms apart plus the room ----------------------------------
    {
      PieceDef clap = MakePiece("Hand Clap", kNoBody, 0.52f, 1.0f);
      Noise(clap, 1.0f, 1150, 1.0f, 0.012f, 500);
      clap.noise.tail = 0.3f; clap.noise.tail_t60 = 0.2f;
      Rattle(clap, 1.0f, 105, 0, 0.35f, 3.0f, 3);
      clap.rattle.to_noise = true;
      Set(39, Stroke("Hand Clap", Add(clap)));
    }

    // ---- Toms: two-headed, floor 16" to rack 10" ---------------------------------------------
    {
      const struct { int note; const char* name; const char* piece; float f, air, t60, pan; } toms[] = {
          {41, "Low Floor Tom", "Floor Tom 16\"", 80, 0.72f, 1.15f, 0.72f},
          {43, "High Floor Tom", "Floor Tom 14\"", 96, 0.64f, 1.0f, 0.66f},
          {45, "Low Tom", "Rack Tom 13\"", 112, 0.58f, 0.9f, 0.58f},
          {47, "Low-Mid Tom", "Rack Tom 12\"", 128, 0.54f, 0.82f, 0.5f},
          {48, "Hi-Mid Tom", "Rack Tom 11\"", 147, 0.5f, 0.75f, 0.44f},
          {50, "High Tom", "Rack Tom 10\"", 168, 0.46f, 0.68f, 0.38f}};
      for (auto& t : toms) {
        PieceDef tom = Drum(t.piece, t.pan, 0.85f, t.f, t.air, 0.15f, 7.5f, t.t60, 0.85f, 2.5f * t.f, 0.4f, 0.005f, 0.16f);
        Reso(tom, 1.04f, 0.45f, 0.8f * t.t60);
        Articulation& a = Set(t.note, Stroke(t.name, Add(tom)));
        a.pos = 0.3f; a.pos_jitter = 0.06f; a.contact_ms = 0.6f;
        Click(a, 0.55f, 2800, 0.6f, 14);
      }
    }

    // ---- Hi-hat: one pair of 14" cymbals; the stroke sets how tightly they are held ----------
    {
      PieceDef hat = Cymbal("Hi-Hat 14\"", 0.34f, 1.25f, 380, 17500, 168, 11, 3800, 12500, 0.6f, 2.4f, 0.9f,
                            0.6f, 3500, 3, 0.4f);
      int h = Add(hat);
      Articulation& closed = Set(42, Stroke("Closed Hi-Hat", h));
      closed.pos = 0; closed.pos_jitter = 0.02f; closed.contact_ms = 0.12f; closed.damp = 0.08f; closed.cascade = 0.6f;
      Click(closed, 0.45f, 8500, 0.7f, 6);
      Articulation& pedal = Set(44, Stroke("Pedal Hi-Hat", h));  // the two cymbals clap shut
      pedal.pos = 0; pedal.contact_ms = 1.1f; pedal.drive = 1.6f; pedal.damp = 0.06f; pedal.cascade = 0.4f;
      pedal.gain = 1.5f;
      Click(pedal, 1.4f, 2600, 0.8f, 9);
      Articulation& open = Set(46, Stroke("Open Hi-Hat", h));
      open.pos = 0; open.contact_ms = 0.14f; open.damp = 0.8f; open.cascade = 1.4f;
      Click(open, 0.4f, 8000, 0.7f, 6);
    }

    // ---- Cymbals ---------------------------------------------------------------------------------
    {
      int c1 = Add(Cymbal("Crash 16\"", 0.36f, 0.9f, 230, 18000, 232, 21, 2600, 9500, 0.6f, 6.0f, 2.0f, 1.2f, 2500, 15, 0.3f));
      Articulation& a = Set(49, Stroke("Crash Cymbal 1", c1));
      a.pos = 0; a.contact_ms = 0.18f;
      Click(a, 0.3f, 6000, 0.7f, 5);
      int c2 = Add(Cymbal("Crash 18\"", 0.64f, 0.9f, 200, 18000, 232, 37, 2300, 8500, 0.6f, 6.5f, 2.2f, 1.2f, 2300, 18, 0.35f));
      Articulation& b = Set(57, Stroke("Crash Cymbal 2", c2));
      b.pos = 0; b.contact_ms = 0.2f;
      Click(b, 0.3f, 5600, 0.7f, 5);
      int sp = Add(Cymbal("Splash 10\"", 0.42f, 0.8f, 520, 18500, 168, 53, 4200, 11500, 0.6f, 1.8f, 0.7f, 1.0f, 3500, 8, 0.2f));
      Articulation& c = Set(55, Stroke("Splash Cymbal", sp));
      c.pos = 0; c.contact_ms = 0.15f;
      Click(c, 0.3f, 7000, 0.7f, 4);
      int ch = Add(Cymbal("China 18\"", 0.7f, 0.8f, 240, 17000, 232, 71, 1500, 7000, 1.0f, 3.0f, 1.0f, 2.0f, 1200, 10, 0.35f));
      Articulation& d = Set(52, Stroke("Chinese Cymbal", ch));
      d.pos = 0; d.contact_ms = 0.2f;
      Click(d, 0.4f, 4000, 0.7f, 5);

      PieceDef ride = Cymbal("Ride 20\"", 0.68f, 0.5f, 250, 17000, 224, 91, 3000, 9000, 0.55f, 8.0f, 2.8f, 0.25f, 3000, 30, 0.5f);
      ride.metal.bell_hz = 620; ride.metal.bell_t60 = 6; ride.metal.bell_count = 6;
      int r1 = Add(ride);
      Articulation& bow = Set(51, Stroke("Ride Cymbal 1", r1));
      bow.pos = 0; bow.pos_jitter = 0.02f; bow.contact_ms = 0.15f;
      Click(bow, 0.6f, 5200, 1.2f, 10);
      Articulation& bell = Set(53, Stroke("Ride Bell", r1));
      bell.pos = 1; bell.pos_jitter = 0; bell.contact_ms = 0.2f; bell.gain = 0.7f; bell.cascade = 0.5f;
      Click(bell, 0.4f, 3200, 1.2f, 8);
      PieceDef ride2 = Cymbal("Ride 22\"", 0.72f, 0.5f, 230, 17000, 224, 103, 2800, 8500, 0.55f, 9.0f, 3.0f, 0.22f, 3000, 35, 0.5f);
      ride2.metal.bell_hz = 560; ride2.metal.bell_t60 = 7; ride2.metal.bell_count = 6;
      Articulation& r2 = Set(59, Stroke("Ride Cymbal 2", Add(ride2)));
      r2.pos = 0; r2.contact_ms = 0.16f;
      Click(r2, 0.55f, 5000, 1.2f, 10);
    }

    // ---- Hand percussion ---------------------------------------------------------------------------
    {
      PieceDef tamb = Modes("Tambourine", 0.58f, 0.8f, {{5300, 1.0f, 0.3f}, {7400, 0.8f, 0.25f}, {9100, 0.7f, 0.22f},
                                                       {11300, 0.5f, 0.18f}, {3150, 0.4f, 0.25f}, {6300, 0.6f, 0.26f},
                                                       {8300, 0.5f, 0.2f}, {12600, 0.3f, 0.15f}});
      Rattle(tamb, 0.8f, 260, 1, 1, 0.25f);
      Noise(tamb, 0.25f, 6500, 0.5f, 0.12f);
      Articulation& a = Set(54, Stroke("Tambourine", Add(tamb)));
      a.contact_ms = 0.4f;

      PieceDef cow = Modes("Cowbell", 0.45f, 0.8f, {{562, 1.0f, 0.3f}, {845, 0.8f, 0.25f}, {1650, 0.3f, 0.12f},
                                                    {2400, 0.15f, 0.08f}, {3350, 0.08f, 0.05f}});
      Articulation& b = Set(56, Stroke("Cowbell", Add(cow)));
      b.contact_ms = 0.4f;
      Click(b, 0.25f, 2200, 0.8f, 4);

      PieceDef slap = Modes("Vibraslap", 0.6f, 0.8f, {{2150, 1.0f, 0.12f}, {3300, 0.7f, 0.1f}, {4800, 0.5f, 0.08f},
                                                      {1250, 0.5f, 0.15f}, {6100, 0.3f, 0.06f}});
      Rattle(slap, 1.0f, 33, 0.3f, 0.25f, 1.1f);
      Noise(slap, 0.2f, 3000, 1.5f, 0.05f);
      Articulation& c = Set(58, Stroke("Vibraslap", Add(slap)));
      c.drive = 0.3f;

      const struct { int note; const char* name; float f, t60, pan; } bongos[] = {
          {60, "Hi Bongo", 395, 0.32f, 0.58f}, {61, "Low Bongo", 290, 0.42f, 0.62f}};
      for (auto& bg : bongos) {
        Articulation& d = Set(bg.note, Stroke(bg.name, Add(Drum(bg.name, bg.pan, 0.8f, bg.f, 0.3f, 0.3f, 5.0f, bg.t60, 0.7f,
                                                               3 * bg.f, 0.4f, 0.004f, 0.04f))));
        d.pos = 0.6f; d.contact_ms = 1.2f; d.contact_exp = 0.25f;
        Click(d, 0.3f, 2600, 0.7f, 5);
      }
      int hc = Add(Drum("Conga (quinto)", 0.55f, 0.85f, 335, 0.35f, 0.35f, 5.0f, 0.5f, 0.7f, 3 * 335, 0.45f, 0.004f, 0.05f));
      Articulation& e = Set(62, Stroke("Mute Hi Conga", hc));
      e.pos = 0.7f; e.contact_ms = 1.6f; e.damp = 0.15f;
      Click(e, 0.35f, 2200, 0.7f, 5);
      Articulation& f = Set(63, Stroke("Open Hi Conga", hc));
      f.pos = 0.75f; f.contact_ms = 1.6f;
      Click(f, 0.25f, 2200, 0.7f, 5);
      Articulation& g = Set(64, Stroke("Low Conga", Add(Drum("Conga (tumba)", 0.65f, 0.85f, 222, 0.4f, 0.35f, 5.0f, 0.65f,
                                                             0.7f, 3 * 222, 0.45f, 0.004f, 0.05f))));
      g.pos = 0.75f; g.contact_ms = 1.8f;
      Click(g, 0.25f, 2000, 0.7f, 5);

      const struct { int note; const char* name; float f, pan; } timbales[] = {
          {65, "High Timbale", 365, 0.4f}, {66, "Low Timbale", 262, 0.45f}};
      for (auto& tb : timbales) {
        PieceDef t = Drum(tb.name, tb.pan, 0.8f, tb.f, 0.3f, 0.3f, 6.0f, 0.7f, 0.7f, 4 * tb.f, 0.3f, 0.004f, 0.02f);
        Reso(t, 2.7f, 0.5f, 0.5f);  // the steel shell rings
        Articulation& h = Set(tb.note, Stroke(tb.name, Add(t)));
        h.pos = 0.6f; h.contact_ms = 0.4f;
        Click(h, 0.5f, 3500, 0.7f, 6);
      }

      PieceDef agogo = Modes("High Agogo", 0.4f, 0.7f, {{915, 1.0f, 0.5f}, {2300, 0.5f, 0.3f}, {4150, 0.2f, 0.2f}, {6100, 0.1f, 0.12f}});
      Articulation& i = Set(67, Stroke("High Agogo", Add(agogo)));
      i.contact_ms = 0.35f;
      for (int m = 0; m < agogo.list.count; ++m) agogo.list.modes[m].freq *= 0.72f;
      agogo.name = "Low Agogo";
      Articulation& j = Set(68, Stroke("Low Agogo", Add(agogo)));
      j.contact_ms = 0.35f;

      // Shakers: PhISEM - beads collide at a rate that follows the shaking energy.
      PieceDef cabasa = Modes("Cabasa", 0.62f, 0.9f, {{3000, 1.0f, 0.03f}, {6200, 0.6f, 0.02f}});
      Rattle(cabasa, 0.35f, 3000, 1, 1, 0.12f);
      Articulation& k = Set(69, Stroke("Cabasa", Add(cabasa)));
      k.drive = 0.2f;
      PieceDef maracas = Modes("Maracas", 0.4f, 0.9f, {{3200, 1.0f, 0.02f}, {5300, 0.4f, 0.015f}});
      Rattle(maracas, 0.4f, 1500, 1, 1, 0.07f);
      Articulation& l = Set(70, Stroke("Maracas", Add(maracas)));
      l.drive = 0.2f;
      PieceDef shaker = Modes("Shaker", 0.6f, 0.9f, {{4500, 1.0f, 0.02f}, {7000, 0.6f, 0.015f}});
      Rattle(shaker, 0.35f, 2500, 1, 1, 0.1f);
      Articulation& m = Set(82, Stroke("Shaker", Add(shaker)));
      m.drive = 0.2f;

      PieceDef whistle = MakePiece("Whistle", kNoBody, 0.5f, 0.6f);
      Tone(whistle, 1.0f, 2650, 0.1f, 0.02f, 0.03f, 20, 28, 0.025f, 8, 90, 0.05f);
      Noise(whistle, 0.08f, 2650, 2.0f, 0.15f);
      int w = Add(whistle);
      Set(71, Stroke("Short Whistle", w)).drive = 0;
      Articulation& lw = Set(72, Stroke("Long Whistle", w));
      lw.drive = 0; lw.length = 4.5f;

      PieceDef guiro = Modes("Guiro", 0.55f, 0.9f, {{1800, 1.0f, 0.04f}, {3700, 0.6f, 0.03f}, {620, 0.4f, 0.05f}});
      Rattle(guiro, 0.8f, 180, 0, 0.2f, 0.18f);
      Noise(guiro, 0.15f, 3000, 0.8f, 0.1f);
      int gu = Add(guiro);
      Set(73, Stroke("Short Guiro", gu)).drive = 0.3f;
      Articulation& lg = Set(74, Stroke("Long Guiro", gu));
      lg.drive = 0.3f; lg.length = 3.0f;

      PieceDef claves = Modes("Claves", 0.6f, 0.8f, {{2480, 1.0f, 0.18f}, {6600, 0.15f, 0.06f}});
      Articulation& n = Set(75, Stroke("Claves", Add(claves)));
      n.contact_ms = 0.2f;
      Click(n, 0.2f, 4000, 0.7f, 3);
      PieceDef block = Modes("Hi Wood Block", 0.45f, 0.8f, {{1300, 1.0f, 0.09f}, {3420, 0.35f, 0.05f}, {5800, 0.1f, 0.03f}});
      Articulation& o = Set(76, Stroke("Hi Wood Block", Add(block)));
      o.contact_ms = 0.25f;
      Click(o, 0.25f, 3000, 0.7f, 3);
      for (int q = 0; q < block.list.count; ++q) block.list.modes[q].freq *= 0.72f;
      block.name = "Low Wood Block"; block.pan = 0.5f;
      Articulation& pq = Set(77, Stroke("Low Wood Block", Add(block)));
      pq.contact_ms = 0.25f;
      Click(pq, 0.25f, 2500, 0.7f, 3);

      PieceDef cuica = MakePiece("Cuica", kNoBody, 0.55f, 0.6f);
      Tone(cuica, 1.0f, 520, 0.35f, 0.15f, 0.4f, 60, 0, 0, 10, 60, 0.08f);
      int cu = Add(cuica);
      Articulation& mc = Set(78, Stroke("Mute Cuica", cu));
      mc.drive = 0; mc.pitch = 1.35f;
      Articulation& oc = Set(79, Stroke("Open Cuica", cu));
      oc.drive = 0; oc.length = 3.0f;

      PieceDef tri = Modes("Triangle", 0.62f, 0.55f, {{1660, 0.6f, 3.0f}, {2560, 1.0f, 2.6f}, {3900, 0.8f, 2.2f},
                                                       {5200, 0.7f, 1.9f}, {6600, 0.6f, 1.6f}, {8100, 0.5f, 1.3f},
                                                       {9800, 0.4f, 1.1f}, {11600, 0.3f, 0.9f}});
      int tr = Add(tri);
      Articulation& mt = Set(80, Stroke("Mute Triangle", tr));
      mt.contact_ms = 0.15f; mt.damp = 0.04f;
      Articulation& ot = Set(81, Stroke("Open Triangle", tr));
      ot.contact_ms = 0.15f;
    }

    // ---- GM2 extras at the edges of the map ---------------------------------------------------
    {
      PieceDef q = MakePiece("High Q", kNoBody, 0.5f, 0.6f);
      Tone(q, 1.0f, 1100, 0.2f, 0.05f, 1.5f, 12, 0, 0, 1, 20, 0.05f);
      Set(27, Stroke("High Q", Add(q))).drive = 0;
      PieceDef slap = Modes("Slap", 0.5f, 0.8f, {{180, 1.0f, 0.06f}, {420, 0.5f, 0.04f}});
      Noise(slap, 1.0f, 1800, 0.8f, 0.05f);
      Articulation& s = Set(28, Stroke("Slap", Add(slap)));
      s.contact_ms = 1.0f;
      PieceDef scratch = MakePiece("Scratch", kNoBody, 0.5f, 0.8f);
      Noise(scratch, 1.0f, 1500, 2.0f, 0.15f);
      scratch.noise.sweep = 1.2f; scratch.noise.sweep_ms = 60;
      int sc = Add(scratch);
      Set(29, Stroke("Scratch Push", sc)).drive = 0;
      Articulation& pull = Set(30, Stroke("Scratch Pull", sc));
      pull.drive = 0; pull.pitch = 0.75f;
      PieceDef sticks = Modes("Sticks", 0.5f, 0.8f, {{2900, 1.0f, 0.05f}, {5100, 0.4f, 0.03f}});
      Articulation& st = Set(31, Stroke("Sticks", Add(sticks)));
      st.contact_ms = 0.2f;
      Click(st, 0.4f, 4000, 0.7f, 3);
      PieceDef square = MakePiece("Square Click", kNoBody, 0.5f, 0.8f);
      Articulation& sq = Set(32, Stroke("Square Click", Add(square)));
      sq.drive = 0;
      Click(sq, 1.0f, 3000, 0.5f, 3);
      PieceDef met = Modes("Metronome Click", 0.5f, 0.8f, {{2000, 1.0f, 0.025f}, {5200, 0.3f, 0.015f}});
      Articulation& mk = Set(33, Stroke("Metronome Click", Add(met)));
      mk.contact_ms = 0.2f;
      PieceDef bell = Modes("Metronome Bell", 0.5f, 0.6f, {{2600, 1.0f, 0.5f}, {7165, 0.4f, 0.25f}});
      Set(34, Stroke("Metronome Bell", Add(bell))).contact_ms = 0.2f;

      PieceDef jingle = Modes("Jingle Bell", 0.6f, 0.7f, {{3400, 1.0f, 0.4f}, {4700, 0.7f, 0.3f}, {6100, 0.6f, 0.25f},
                                                         {8200, 0.4f, 0.2f}, {9700, 0.3f, 0.18f}});
      Rattle(jingle, 0.6f, 300, 1, 1, 0.3f);
      Set(83, Stroke("Jingle Bell", Add(jingle))).contact_ms = 0.3f;
      PieceDef tree = Modes("Bell Tree", 0.55f, 0.5f, {});
      for (int b = 0; b < 12; ++b) tree.list.modes[tree.list.count++] = {6200.0f * powf(0.9f, float(b)), 1.0f, 1.4f};
      Rattle(tree, 0.7f, 28, 0, 0.1f, 20.0f, 12);
      tree.rattle.sequential = true;
      Articulation& bt = Set(84, Stroke("Bell Tree", Add(tree)));
      bt.drive = 0.0f;
      PieceDef cast = Modes("Castanets", 0.45f, 0.8f, {{1900, 1.0f, 0.035f}, {4300, 0.5f, 0.025f}, {780, 0.4f, 0.03f}});
      Set(85, Stroke("Castanets", Add(cast))).contact_ms = 0.2f;
      PieceDef surdo = Drum("Surdo", 0.5f, 0.9f, 62, 1.0f, 0.25f, 6.0f, 1.3f, 0.8f, 150, 0.5f, 0.004f, 0.07f);
      Reso(surdo, 1.03f, 0.5f, 1.0f);
      int su = Add(surdo);
      Articulation& ms = Set(86, Stroke("Mute Surdo", su));
      ms.pos = 0.3f; ms.contact_ms = 2.5f; ms.damp = 0.12f;
      Articulation& os = Set(87, Stroke("Open Surdo", su));
      os.pos = 0.3f; os.contact_ms = 2.5f;
    }
  }
};

const KitTable& Kit() {
  static const KitTable table;
  return table;
}

// ============================================================================================
// Runtime
// ============================================================================================

struct Biquad {
  float b0, b1, b2, a1, a2, z1, z2;
  float Process(float x) {
    float y = b0 * x + z1;
    z1 = b1 * x - a1 * y + z2;
    z2 = b2 * x - a2 * y;
    return y;
  }
  void Reset() { z1 = z2 = 0; }
  // RBJ cookbook; band-pass has 0 dB peak gain.
  void Set(int type, double hz, double q, double sr) {
    const double w0 = 2.0 * kPi * std::min(hz, 0.45 * sr) / sr;
    const double alpha = sin(w0) / (2.0 * std::max(0.1, q)), cw = cos(w0), a0 = 1.0 + alpha;
    double nb0, nb1, nb2;
    if (type == 0) { nb0 = alpha; nb1 = 0; nb2 = -alpha; }                                  // band-pass
    else if (type == 1) { nb0 = (1 + cw) / 2; nb1 = -(1 + cw); nb2 = (1 + cw) / 2; }        // high-pass
    else { nb0 = (1 - cw) / 2; nb1 = 1 - cw; nb2 = (1 - cw) / 2; }                          // low-pass
    b0 = float(nb0 / a0); b1 = float(nb1 / a0); b2 = float(nb2 / a0);
    a1 = float(-2.0 * cw / a0); a2 = float((1.0 - alpha) / a0);
  }
  void SetBandPass(double hz, double q, double sr) { Set(0, hz, q, sr); }
  void SetHighPass(double hz, double sr) { Set(1, hz, 0.7071, sr); }
  void SetLowPass(double hz, double sr) { Set(2, hz, 0.7071, sr); }
};

// Modes as structure-of-arrays, padded to a multiple of 4 for SSE.
struct Bank {
  int count = 0, n = 0;
  float* mem = nullptr;
  float *re, *im, *c, *s;  // phasor state and rotation (rotation includes the decay r)
  float *cw, *sw, *r;      // unit rotation at the current glide, current decay
  float *w, *t60;          // base radian frequency per sample, base T60
  float *base, *g, *h;     // static level; per-hit drive gain; cascade drive gain
  float *casc, *bw;        // cascade weight; snare-side head weight (wires)
  float *mix_a, *mix_b;    // metal: edge/bow and bell levels
  float* pos_table;        // membrane: J_m(j_mn r) at kPositions radii (n * kPositions)
  std::vector<int> m_index;  // membrane circumferential order, -1 = resonant head / other

  void Allocate(int modes) {
    count = modes;
    n = (modes + 3) & ~3;
    if (n == 0) n = 4;
    const int arrays = 16;
    mem = static_cast<float*>(_mm_malloc(sizeof(float) * n * (arrays + kPositions), 16));
    memset(mem, 0, sizeof(float) * n * (arrays + kPositions));
    float* p = mem;
    float** fields[] = {&re, &im, &c, &s, &cw, &sw, &r, &w, &t60, &base, &g, &h, &casc, &bw, &mix_a};
    for (float** f : fields) { *f = p; p += n; }
    mix_b = p; p += n;
    pos_table = p;
    m_index.assign(n, -1);
  }
  void Free() {
    if (mem) _mm_free(mem);
    mem = nullptr;
    count = n = 0;
  }
};

struct Piece {
  const PieceDef* def = nullptr;
  Bank bank;
  double sr = 48000;
  uint32_t seed = 1, seed0 = 1;  // seed0: restored by Silence() so tool renders repeat exactly
  int owner = -1;
  bool active = false;
  float level = 0;
  int pos = kBlock;
  float buf[kBlock];

  float out_gain = 1;
  float damp = 1;          // current body T60 multiplier
  float glide_kappa = 0;   // pitch = sqrt(1 + kappa * energy)
  float glide_mult = 1;
  float wire_scale = 1;
  float casc_norm = 1;

  // contact pulse
  int strike_t = 0, strike_len = 0;
  float strike_amp = 0;
  // contact noise
  Biquad click_bq = {};
  float click_env = 0, click_coef = 0;
  // cascade
  float casc_amp = 0, casc_d = 0, casc_a = 0, casc_dc = 0, casc_ac = 0;
  // noise layer
  Biquad noise_bq = {}, noise_hp = {};
  float noise_env = 0, noise_coef = 0, noise_tail = 0, noise_tail_coef = 0;
  float noise_sweep = 0, noise_sweep_coef = 0, noise_pitch = 1, noise_level = 0;
  // wires
  Biquad wire_hp = {}, wire_lp = {};
  float wire_gain = 0, wire_gate = 0, wire_gate_coef = 0, wire_ring = 0, wire_ring_coef = 0;
  // rattle
  float rat_energy = 0, rat_coef = 0, rat_phase = 0, rat_amp = 0;
  int rat_events = 0;
  // tone
  float tone_amp = 0, tone_phase = 0, tone_warble = 0, tone_pitch = 1, tone_hold = 0;
  int tone_t = -1;

  float Random() {  // xorshift32, [-1, 1)
    seed ^= seed << 13; seed ^= seed >> 17; seed ^= seed << 5;
    return float(int32_t(seed)) * (1.0f / 2147483648.0f);
  }
  float Random01() { return 0.5f + 0.5f * Random(); }
  float Gauss() { return (Random() + Random() + Random()) * 1.0f; }  // ~N(0, 1)
};

uint32_t Hash(uint32_t x) {
  x ^= x >> 16; x *= 0x7feb352d; x ^= x >> 15; x *= 0x846ca68b; x ^= x >> 16;
  return x ? x : 1;
}

const int kBesselOrders = 25, kBesselZeros = 8;

// n-th positive zero of J_m (m < 25, n <= 8), computed once for the whole process.
double BesselZero(int m, int n) {
  struct Table {
    double z[kBesselOrders][kBesselZeros] = {};
    Table() {
      for (int m = 0; m < kBesselOrders; ++m) {
        double x = std::max(0.5, double(m)), prev = std::cyl_bessel_j(double(m), x);
        for (int found = 0; found < kBesselZeros && x < 80; x += 0.05) {
          double v = std::cyl_bessel_j(double(m), x + 0.05);
          if ((prev < 0) != (v < 0)) {
            double lo = x, hi = x + 0.05;
            const bool lo_negative = prev < 0;
            for (int i = 0; i < 40; ++i) {
              double mid = 0.5 * (lo + hi);
              if ((std::cyl_bessel_j(double(m), mid) < 0) == lo_negative) lo = mid;
              else hi = mid;
            }
            z[m][found++] = 0.5 * (lo + hi);
          }
          prev = v;
        }
      }
    }
  };
  static const Table table;
  return m >= 0 && m < kBesselOrders && n >= 1 && n <= kBesselZeros ? table.z[m][n - 1] : 0;
}

// ---- building the bodies ------------------------------------------------------------------

struct ModeSpec { float f, t60, base, casc, bw, mix_a, mix_b; int m; float shape[kPositions]; };

void BuildMembrane(const MembraneDef& d, double sr, uint32_t seed, std::vector<ModeSpec>& out) {
  struct Z { int m, n; double j; };
  std::vector<Z> zeros;
  for (int m = 0; m <= 24; ++m)
    for (int n = 1; n <= 8; ++n) {
      double j = BesselZero(m, n);
      if (j > 0) zeros.push_back({m, n, j});
    }
  auto loaded = [&](const Z& z) {
    double f = z.j / sqrt(1.0 + d.air / z.j);
    if (z.m == 0) f *= sqrt(1.0 + d.cavity / (z.n * z.n));
    return f;
  };
  const double ref = loaded({0, 1, BesselZero(0, 1)});
  std::vector<std::pair<double, Z>> modes;
  for (const Z& z : zeros) {
    double ratio = loaded(z) / ref;
    if (ratio <= d.max_ratio && d.f01 * ratio < 0.45 * sr) modes.push_back({ratio, z});
  }
  std::sort(modes.begin(), modes.end(), [](auto& a, auto& b) { return a.first < b.first; });
  uint32_t rng = seed;
  auto rnd = [&]() { rng = Hash(rng + 0x9e3779b9u); return (rng & 0xffffff) / float(0x1000000); };
  for (auto& [ratio, z] : modes) {
    if (int(out.size()) >= kMaxMembraneModes) break;
    ModeSpec s = {};
    s.f = float(d.f01 * ratio);
    s.m = z.m;
    s.t60 = float(d.t60 / (1.0 + std::max(0.0, s.f - double(d.f01)) / d.damp_hz) * (z.m == 0 ? d.t60_axisym : 1.0f));
    const double norm = 1.0 / std::max(0.05, fabs(std::cyl_bessel_j(double(z.m + 1), z.j))) * (z.m == 0 ? 0.7071 : 1.0);
    const double rad = z.m == 0 ? 1.0 : z.m == 1 ? 0.7 : 0.5;
    s.base = float(norm * rad * pow(1.0 / ratio, d.tilt));
    s.bw = z.m == 0 ? 0.5f : z.m == 1 ? 0.15f : 0.03f;
    for (int i = 0; i < kPositions; ++i) s.shape[i] = float(std::cyl_bessel_j(double(z.m), z.j * i / (kPositions - 1.0)));
    out.push_back(s);
    // A real head is never perfectly uniform: each (m>0) pair splits slightly and beats.
    if (d.split > 0 && z.m > 0 && ratio < 5 && int(out.size()) < kMaxMembraneModes) {
      ModeSpec t = s;
      t.f = float(s.f * (1.0 + d.split * (0.5 + rnd())));
      t.base = s.base * 0.35f;
      out.push_back(t);
    }
  }
  if (d.reso_ratio > 0) {  // resonant head: excited through the enclosed air
    for (int m = 0; m <= 2; ++m)
      for (int n = 1; n <= 2; ++n) {
        Z z = {m, n, BesselZero(m, n)};
        double ratio = loaded(z) / ref;
        double f = d.f01 * d.reso_ratio * ratio;
        if (f >= 0.45 * sr) continue;
        ModeSpec s = {};
        s.f = float(f);
        s.m = -1;
        s.t60 = float(d.reso_t60 / (1.0 + std::max(0.0, f - d.f01 * d.reso_ratio) / d.damp_hz));
        s.base = float(d.reso_gain * (m == 0 ? 1.0 : 0.5) * pow(1.0 / ratio, d.tilt) * (n == 1 ? 1.0 : 0.5));
        s.bw = m == 0 ? 1.0f : m == 1 ? 0.5f : 0.2f;
        for (int i = 0; i < kPositions; ++i) s.shape[i] = 1;
        out.push_back(s);
      }
  }
}

void BuildMetal(const MetalDef& d, double sr, std::vector<ModeSpec>& out) {
  uint32_t rng = Hash(d.seed);
  auto rnd = [&]() { rng = Hash(rng + 0x9e3779b9u); return (rng & 0xffffff) / float(0x1000000); };
  auto gauss = [&]() { return (rnd() + rnd() + rnd() - 1.5f) * 2.0f; };
  const double s_lo = sqrt(d.f_lo), s_hi = sqrt(std::min(double(d.f_hi), 0.45 * sr));
  // Mode density grows ~sqrt(f) with frequency: denser than log spacing up high, sparser low.
  for (int k = 0; k < d.count; ++k) {
    double u = (k + 0.5 + 0.9 * (rnd() - 0.5)) / d.count;
    double sq = s_lo + u * (s_hi - s_lo);
    double f = sq * sq;
    ModeSpec s = {};
    s.f = float(f);
    s.m = -1;
    double env = 1.0 / (1.0 + pow(d.peak_lo / f, 2.0)) / sqrt(1.0 + pow(f / d.peak_hi, 2.0));
    s.base = float(env * exp(d.spread * gauss()));
    double t = log(f / d.f_lo) / log(d.f_hi / d.f_lo);
    s.t60 = float(d.t60_lo * pow(d.t60_hi / d.t60_lo, std::clamp(t, 0.0, 1.0)) * (0.7 + 0.6 * rnd()));
    s.casc = float(f > d.cascade_hz ? 1.0 : pow(f / d.cascade_hz, 3.0));
    s.mix_a = 1.0f;
    s.mix_b = 0.25f;
    out.push_back(s);
  }
  static const float bell_ratio[] = {1.0f, 2.08f, 2.95f, 4.02f, 5.16f, 6.41f, 7.7f, 9.1f};
  for (int b = 0; b < d.bell_count && b < 8; ++b) {
    double f = d.bell_hz * bell_ratio[b];
    if (f >= 0.45 * sr) break;
    ModeSpec s = {};
    s.f = float(f);
    s.m = -1;
    s.t60 = d.bell_t60 / (1.0f + 0.25f * b);
    s.base = 4.0f / (1.0f + 0.4f * b);
    s.mix_a = 0.04f;
    s.mix_b = 1.0f;
    out.push_back(s);
  }
}

// Per-hit drive gains for a stroke at `pos` (no randomness): used for normalisation.
void StrikeGains(Piece& p, float pos, float* g) {
  const Bank& b = p.bank;
  const BodyType body = p.def->body;
  const float x = std::clamp(pos, 0.0f, 1.0f) * (kPositions - 1);
  const int i0 = std::min(int(x), kPositions - 2);
  const float frac = x - i0;
  for (int k = 0; k < b.count; ++k) {
    float shape = 1;
    if (body == kMembraneBody) {
      const float* t = b.pos_table + k * kPositions;
      shape = t[i0] + (t[i0 + 1] - t[i0]) * frac;
    } else if (body == kMetalBody) {
      shape = b.mix_a[k] * (1 - pos) + b.mix_b[k] * pos;
    }
    g[k] = b.base[k] * shape;
  }
}

void UpdateRotation(Piece& p) {
  Bank& b = p.bank;
  const float nyquist = float(kPi * 0.95);
  for (int k = 0; k < b.count; ++k) {
    float w = std::min(b.w[k] * p.glide_mult, nyquist);
    b.cw[k] = cosf(w);
    b.sw[k] = sinf(w);
    b.c[k] = b.r[k] * b.cw[k];
    b.s[k] = b.r[k] * b.sw[k];
  }
}

void SetDamp(Piece& p, float damp) {
  Bank& b = p.bank;
  p.damp = damp;
  for (int k = 0; k < b.count; ++k) {
    float t60 = std::max(0.003f, b.t60[k] * damp);
    float r = float(exp(-kLn1000 / (t60 * p.sr)));
    b.r[k] = r;
    b.c[k] = r * b.cw[k];
    b.s[k] = r * b.sw[k];
    // Noise drive scaled so each mode's response to it doesn't depend on its decay.
    b.h[k] = b.casc[k] * sqrtf(std::max(0.0f, 1 - r * r)) * p.casc_norm;
  }
}

// ---- rendering ------------------------------------------------------------------------------

template <bool kDrive, bool kWires>
void RenderModes(Bank& b, const float* x, const float* xc, float* out, float* yb) {
  __m128 acc[kBlock], accb[kBlock];
  for (int i = 0; i < kBlock; ++i) {
    acc[i] = _mm_setzero_ps();
    if (kWires) accb[i] = _mm_setzero_ps();
  }
  const __m128 abs_mask = _mm_castsi128_ps(_mm_set1_epi32(0x7fffffff));
  const __m128 tiny = _mm_set1_ps(1e-20f);
  for (int k = 0; k < b.n; k += 4) {
    __m128 re = _mm_load_ps(b.re + k), im = _mm_load_ps(b.im + k);
    const __m128 c = _mm_load_ps(b.c + k), s = _mm_load_ps(b.s + k);
    __m128 g = _mm_setzero_ps(), h = _mm_setzero_ps(), bw = _mm_setzero_ps();
    if (kDrive) { g = _mm_load_ps(b.g + k); h = _mm_load_ps(b.h + k); }
    if (kWires) bw = _mm_load_ps(b.bw + k);
    for (int i = 0; i < kBlock; ++i) {
      __m128 nre = _mm_sub_ps(_mm_mul_ps(re, c), _mm_mul_ps(im, s));
      im = _mm_add_ps(_mm_mul_ps(re, s), _mm_mul_ps(im, c));
      if (kDrive)
        nre = _mm_add_ps(nre, _mm_add_ps(_mm_mul_ps(g, _mm_set1_ps(x[i])), _mm_mul_ps(h, _mm_set1_ps(xc[i]))));
      re = nre;
      acc[i] = _mm_add_ps(acc[i], im);
      if (kWires) accb[i] = _mm_add_ps(accb[i], _mm_mul_ps(im, bw));
    }
    // Flush modes that have died away before they become denormals.
    __m128 dead = _mm_cmplt_ps(_mm_add_ps(_mm_and_ps(re, abs_mask), _mm_and_ps(im, abs_mask)), tiny);
    _mm_store_ps(b.re + k, _mm_andnot_ps(dead, re));
    _mm_store_ps(b.im + k, _mm_andnot_ps(dead, im));
  }
  for (int i = 0; i < kBlock; ++i) {
    __m128 v = _mm_add_ps(acc[i], _mm_movehl_ps(acc[i], acc[i]));
    v = _mm_add_ss(v, _mm_shuffle_ps(v, v, 1));
    out[i] = _mm_cvtss_f32(v);
    if (kWires) {
      __m128 u = _mm_add_ps(accb[i], _mm_movehl_ps(accb[i], accb[i]));
      u = _mm_add_ss(u, _mm_shuffle_ps(u, u, 1));
      yb[i] = _mm_cvtss_f32(u);
    }
  }
}

float Energy(const Bank& b) {
  __m128 e = _mm_setzero_ps();
  for (int k = 0; k < b.n; k += 4) {
    __m128 re = _mm_load_ps(b.re + k), im = _mm_load_ps(b.im + k);
    e = _mm_add_ps(e, _mm_add_ps(_mm_mul_ps(re, re), _mm_mul_ps(im, im)));
  }
  e = _mm_add_ps(e, _mm_movehl_ps(e, e));
  e = _mm_add_ss(e, _mm_shuffle_ps(e, e, 1));
  return _mm_cvtss_f32(e);
}

void RenderPiece(Piece& p) {
  const PieceDef& d = *p.def;
  const float sr = float(p.sr);
  alignas(16) float x[kBlock], xc[kBlock], out[kBlock], yb[kBlock];
  bool drive = false, busy = false;
  memset(x, 0, sizeof(x));
  memset(xc, 0, sizeof(xc));

  // Contact force: a Hertzian half-sine^1.5 pulse.
  if (p.strike_t < p.strike_len) {
    drive = busy = true;
    for (int i = 0; i < kBlock && p.strike_t < p.strike_len; ++i, ++p.strike_t) {
      float sn = sinf(float(kPi) * (p.strike_t + 0.5f) / p.strike_len);
      x[i] = p.strike_amp * sn * sqrtf(sn);
    }
  }
  // Collisions (shakers, jingles, scrape, clap bursts).
  if (p.rat_energy > 1e-4f) {
    busy = true;
    const RattleDef& r = d.rattle;
    for (int i = 0; i < kBlock; ++i) {
      const float rate = r.rate_hz * (r.rate_exp > 0 ? powf(p.rat_energy, r.rate_exp) : 1.0f);
      bool event;
      if (r.randomness >= 1) {
        event = p.Random01() < rate / sr;
      } else {
        p.rat_phase += rate / sr * (1 + r.randomness * p.Random());
        event = p.rat_phase >= 1;
        if (event) p.rat_phase -= 1;
      }
      if (event && (r.max_events == 0 || p.rat_events < r.max_events)) {
        const float rel = sqrtf(p.rat_energy) * (1 - 0.5f * r.randomness * p.Random01());
        const float amp = p.rat_amp * rel;
        if (r.to_noise) {
          p.noise_env = std::max(p.noise_env, rel * p.noise_level);
          p.noise_tail += rel * p.noise_level * d.noise.tail * 0.5f;
        } else if (r.sequential && p.bank.count > 0) {
          const int k = p.rat_events % p.bank.count;
          p.bank.re[k] += amp * p.bank.base[k];
        } else {
          x[i] += amp;
          drive = true;
        }
        ++p.rat_events;
      }
      p.rat_energy *= p.rat_coef;
    }
    if (r.max_events > 0 && p.rat_events >= r.max_events) p.rat_energy = 0;
  }
  // Energy cascade: noise into the upper modes with a rise and a decay.
  if (p.casc_amp > 0) {
    drive = busy = true;
    for (int i = 0; i < kBlock; ++i) {
      xc[i] = p.Random() * p.casc_amp * (p.casc_d - p.casc_a);
      p.casc_d *= p.casc_dc;
      p.casc_a *= p.casc_ac;
    }
    if (p.casc_d < 1e-4f) p.casc_amp = 0;
  }
  // Tension modulation: pitch follows the head's vibration energy.
  if (p.glide_kappa > 0) {
    float m = sqrtf(1 + p.glide_kappa * Energy(p.bank));
    if (fabsf(m - p.glide_mult) > 2e-5f) {
      p.glide_mult = m;
      UpdateRotation(p);
    }
  }

  const bool wires = d.wires.amount > 0 && p.wire_gain > 0;
  if (p.bank.count == 0) memset(out, 0, sizeof(out));
  else if (drive && wires) RenderModes<true, true>(p.bank, x, xc, out, yb);
  else if (drive) RenderModes<true, false>(p.bank, x, xc, out, yb);
  else if (wires) RenderModes<false, true>(p.bank, x, xc, out, yb);
  else RenderModes<false, false>(p.bank, x, xc, out, yb);

  // Snare wires: a one-sided contact with the snare-side head. They only rattle while the head
  // swings away from them by more than their own spring can follow (the threshold), so the buzz
  // is chopped at the head's frequency and stops abruptly once the head has calmed down.
  if (wires) {
    const float thr = d.wires.threshold;
    for (int i = 0; i < kBlock; ++i) {
      float gate = std::max(0.0f, yb[i] * p.wire_scale - thr);
      p.wire_gate += (gate - p.wire_gate) * p.wire_gate_coef;  // the wires' inertia
      // The strands keep chattering a little after each slap: a slower, smoother component.
      p.wire_ring = std::max(gate, p.wire_ring * p.wire_ring_coef);
      float drive = 0.65f * p.wire_gate + 0.35f * p.wire_ring;
      float y = p.wire_lp.Process(p.wire_hp.Process(p.Random() * (drive + 1e-12f)));
      out[i] += y * p.wire_gain;
    }
    busy |= p.wire_gate + p.wire_ring > 1e-5f;
  }
  if (p.click_env > 1e-6f) {
    busy = true;
    for (int i = 0; i < kBlock; ++i) {
      out[i] += p.click_bq.Process(p.Random()) * p.click_env;
      p.click_env *= p.click_coef;
    }
  }
  if (p.noise_env + p.noise_tail > 1e-6f) {
    busy = true;
    if (d.noise.sweep != 0) {
      p.noise_bq.Set(0, d.noise.hz * p.noise_pitch * (1 + d.noise.sweep * p.noise_sweep), d.noise.q, p.sr);
      for (int i = 0; i < kBlock; ++i) p.noise_sweep *= p.noise_sweep_coef;
    }
    for (int i = 0; i < kBlock; ++i) {
      float n = p.Random();
      if (d.noise.hp_hz > 0) n = p.noise_hp.Process(n);
      out[i] += p.noise_bq.Process(n) * (p.noise_env + p.noise_tail);
      p.noise_env *= p.noise_coef;
      p.noise_tail *= p.noise_tail_coef;
    }
  }
  if (p.tone_t >= 0) {
    busy = true;
    const ToneDef& t = d.tone;
    const float attack = std::max(1.0f, t.attack_ms * 0.001f * sr);
    const float decay = kLn1000 / (std::max(0.005f, t.t60) * sr);
    for (int i = 0; i < kBlock; ++i, ++p.tone_t) {
      float env = 1 - expf(-p.tone_t / attack);
      if (p.tone_t > p.tone_hold) env *= expf(-(p.tone_t - p.tone_hold) * decay);
      float f = t.hz * p.tone_pitch * (1 + t.glide * expf(-p.tone_t / (std::max(1.0f, t.glide_ms) * 0.001f * sr)));
      if (t.warble_depth > 0) {
        f *= 1 + t.warble_depth * sinf(p.tone_warble);
        p.tone_warble += float(2 * kPi) * t.warble_hz / sr;
      }
      p.tone_phase += float(2 * kPi) * std::min(f, 0.45f * sr) / sr;
      if (p.tone_phase > float(2 * kPi)) p.tone_phase -= float(2 * kPi);
      out[i] += p.tone_amp * env * (sinf(p.tone_phase) + t.h2 * sinf(2 * p.tone_phase) + t.h3 * sinf(3 * p.tone_phase));
    }
    if (p.tone_t > p.tone_hold + t.t60 * sr) p.tone_t = -1;
  }

  float peak = 0;
  for (int i = 0; i < kBlock; ++i) {
    p.buf[i] = out[i] * p.out_gain;
    peak = std::max(peak, fabsf(p.buf[i]));
  }
  p.level = std::max(peak, p.level * 0.97f);
  if (!busy && p.level < 3e-6f) p.active = false;  // below -110 dB
}

void ResetPiece(Piece& p) {
  Bank& b = p.bank;
  memset(b.re, 0, sizeof(float) * b.n);
  memset(b.im, 0, sizeof(float) * b.n);
  p.level = 0;
  p.pos = kBlock;
  p.strike_t = p.strike_len = 0;
  p.click_env = p.casc_amp = p.noise_env = p.noise_tail = 0;
  p.wire_gate = p.wire_ring = 0;
  p.rat_energy = 0;
  p.tone_t = -1;
  p.click_bq.Reset(); p.noise_bq.Reset(); p.noise_hp.Reset(); p.wire_hp.Reset(); p.wire_lp.Reset();
  if (p.glide_mult != 1) {
    p.glide_mult = 1;
    UpdateRotation(p);
  }
}

void Strike(Piece& p, const Articulation& a, float velocity) {
  const PieceDef& d = *p.def;
  const float sr = float(p.sr);
  const float vel = std::clamp(velocity, 0.02f, 1.0f);
  // Struck-instrument loudness grows faster than the stick speed (~40 dB over the MIDI range);
  // a little round-robin variation in force, place and contact keeps repeats from being identical.
  const float va = powf(vel, 1.6f) * (1 + 0.08f * p.Random()) * a.gain;

  if (a.damp != p.damp) SetDamp(p, a.damp);

  Bank& b = p.bank;
  if (b.count > 0) {
    const float pos = std::clamp(a.pos + a.pos_jitter * p.Random(), 0.0f, 1.0f);
    StrikeGains(p, pos, b.g);
    const float jitter = d.body == kMetalBody ? 0.35f : 0.06f;  // a cymbal is never hit twice in the same spot
    for (int k = 0; k < b.count; ++k) b.g[k] *= expf(jitter * p.Gauss());
    // A plate's mode shapes change sign all over its surface, so at the strike point the modes
    // start with random signs (not all in phase, which would make one huge spike).
    if (d.body == kMetalBody)
      for (int k = 0; k < b.count; ++k)
        if (p.Random() < 0) b.g[k] = -b.g[k];
  }

  // Contact pulse: a harder hit compresses the stick tip / felt more, so contact is shorter
  // and the spectrum reaches higher.
  float tc = a.contact_ms * powf(vel, -a.contact_exp) * (1 + 0.06f * p.Random());
  tc = std::min(tc, 3 * a.contact_ms);
  p.strike_len = std::max(2, int(tc * 0.001f * sr + 0.5f));
  p.strike_t = 0;
  p.strike_amp = va * a.drive / (0.5564f * p.strike_len);  // unit impulse area
  if (a.drive <= 0) p.strike_len = 0;

  if (a.click > 0) {
    p.click_bq.SetBandPass(a.click_hz * (1 + 0.05f * p.Random()), a.click_q, p.sr);
    p.click_env = a.click * va * powf(vel, 0.6f);
    p.click_coef = expf(-kLn1000 / (std::max(0.5f, a.click_ms) * 0.001f * sr));
  }
  if (d.noise.amount > 0) {
    p.noise_pitch = a.pitch;
    p.noise_bq.SetBandPass(d.noise.hz * a.pitch, d.noise.q, p.sr);
    if (d.noise.hp_hz > 0) p.noise_hp.SetHighPass(d.noise.hp_hz, p.sr);
    p.noise_level = d.noise.amount * a.noise * va;
    p.noise_env = p.noise_level;
    p.noise_tail = p.noise_level * d.noise.tail;
    p.noise_coef = expf(-kLn1000 / (std::max(0.002f, d.noise.t60 * a.length) * sr));
    p.noise_tail_coef = expf(-kLn1000 / (std::max(0.002f, d.noise.tail_t60) * sr));
    p.noise_sweep = 1;
    p.noise_sweep_coef = d.noise.sweep != 0 ? expf(-1.0f / (std::max(1.0f, d.noise.sweep_ms) * 0.001f * sr)) : 0;
  }
  if (d.body == kMetalBody && d.metal.cascade > 0 && a.cascade > 0) {
    // Louder hits bloom more: the cascade is nonlinear (~ amplitude squared).
    p.casc_amp = d.metal.cascade * a.cascade * va * vel;
    p.casc_d = 1;
    p.casc_a = 1;
    p.casc_dc = expf(-kLn1000 / (std::max(0.01f, d.metal.cascade_t60 * a.damp) * sr));
    p.casc_ac = expf(-1.0f / (std::max(0.2f, d.metal.cascade_attack_ms) * 0.001f * sr));
  }
  if (d.wires.amount > 0) {
    p.wire_gain = d.wires.amount * a.wires;
    p.wire_gate_coef = 1 - expf(-2 * float(kPi) * 6000 / sr);
    p.wire_ring_coef = expf(-1.0f / (std::max(0.5f, d.wires.ring_ms) * 0.001f * sr));
  }
  if (d.rattle.amount > 0 && a.rattle > 0) {
    p.rat_energy = 1;
    p.rat_coef = expf(-kLn1000 / (std::max(0.01f, d.rattle.t60 * a.length) * sr));
    p.rat_amp = d.rattle.amount * a.rattle * va;
    p.rat_phase = 0;
    p.rat_events = 0;
  }
  if (d.tone.amount > 0 && a.tone > 0) {
    p.tone_t = 0;
    p.tone_amp = d.tone.amount * a.tone * va;
    p.tone_pitch = a.pitch;
    p.tone_hold = d.tone.hold_ms * 0.001f * sr * a.length;
    p.tone_warble = 0;
  }
}

void InitPiece(Piece& p, const PieceDef& d, double sr, uint32_t seed) {
  p.def = &d;
  p.sr = sr;
  p.seed = p.seed0 = Hash(seed);
  std::vector<ModeSpec> specs;
  if (d.body == kMembraneBody) BuildMembrane(d.mem, sr, seed, specs);
  else if (d.body == kMetalBody) BuildMetal(d.metal, sr, specs);
  else if (d.body == kModeListBody) {
    for (int k = 0; k < d.list.count; ++k) {
      const Mode& m = d.list.modes[k];
      if (m.freq <= 0 || m.freq >= 0.45 * sr) continue;
      ModeSpec s = {};
      s.f = m.freq; s.t60 = m.t60; s.base = m.gain; s.m = -1;
      specs.push_back(s);
    }
  }
  Bank& b = p.bank;
  b.Allocate(int(specs.size()));
  for (int k = 0; k < b.count; ++k) {
    const ModeSpec& s = specs[k];
    b.w[k] = float(2 * kPi * s.f / sr);
    b.t60[k] = s.t60;
    b.base[k] = s.base;
    b.casc[k] = s.casc;
    b.bw[k] = s.bw;
    b.mix_a[k] = s.mix_a;
    b.mix_b[k] = s.mix_b;
    b.m_index[k] = s.m;
    memcpy(b.pos_table + k * kPositions, s.shape, sizeof(s.shape));
  }
  // Normalise: a nominal full-velocity stroke leaves unit total mode energy.
  if (b.count > 0) {
    std::vector<float> g(b.n);
    StrikeGains(p, d.body == kMembraneBody ? 0.3f : 0.0f, g.data());
    double e = 0;
    for (int k = 0; k < b.count; ++k) e += double(g[k]) * g[k];
    const float scale = e > 0 ? float(1.0 / sqrt(e)) : 1.0f;
    for (int k = 0; k < b.count; ++k) b.base[k] *= scale;
    // Cascade weights: the same overall scale and spectral envelope as the modes.
    double ce = 0;
    for (int k = 0; k < b.count; ++k) {
      b.casc[k] *= b.base[k];
      ce += double(b.casc[k]) * b.casc[k];
    }
    p.casc_norm = ce > 0 ? float(1.0 / sqrt(ce)) : 0.0f;
    if (d.body == kMembraneBody) {
      // Tension modulation: at the nominal hit's energy (1) the pitch is (1 + glide) x rest.
      p.glide_kappa = (1 + d.mem.glide) * (1 + d.mem.glide) - 1;
      // Wires: the nominal hit's snare-side head displacement scaled to ~1.
      double sum = 0;
      StrikeGains(p, 0.3f, g.data());
      for (int k = 0; k < b.count; ++k) sum += fabs(double(g[k]) * b.bw[k]);
      p.wire_scale = sum > 0 ? float(1.0 / sum) : 1.0f;
    }
  }
  if (d.wires.amount > 0) {
    p.wire_hp.SetHighPass(d.wires.hp_hz, sr);
    p.wire_lp.SetLowPass(d.wires.lp_hz, sr);
  }
  p.glide_mult = 1;
  UpdateRotation(p);
  SetDamp(p, 1);
  p.out_gain = d.gain * kKitGain;
  ResetPiece(p);
}

// Calibratable parameters: float fields of the piece definition or of the articulation.
struct ParamInfo { const char* name; bool piece; size_t offset; };
#define PP(name, field) {name, true, offsetof(PieceDef, field)}
#define PA(name, field) {name, false, offsetof(Articulation, field)}
const ParamInfo kParams[] = {
    PP("f01", mem.f01), PP("air", mem.air), PP("cavity", mem.cavity), PP("max_ratio", mem.max_ratio),
    PP("t60", mem.t60), PP("t60_axisym", mem.t60_axisym), PP("damp_hz", mem.damp_hz), PP("tilt", mem.tilt),
    PP("split", mem.split), PP("glide", mem.glide), PP("reso_ratio", mem.reso_ratio), PP("reso_gain", mem.reso_gain),
    PP("reso_t60", mem.reso_t60),
    PP("f_lo", metal.f_lo), PP("f_hi", metal.f_hi), PP("peak_lo", metal.peak_lo), PP("peak_hi", metal.peak_hi),
    PP("spread", metal.spread), PP("t60_lo", metal.t60_lo), PP("t60_hi", metal.t60_hi), PP("cascade", metal.cascade),
    PP("cascade_hz", metal.cascade_hz), PP("cascade_attack_ms", metal.cascade_attack_ms),
    PP("cascade_t60", metal.cascade_t60), PP("bell_hz", metal.bell_hz), PP("bell_t60", metal.bell_t60),
    PP("noise_amount", noise.amount), PP("noise_hz", noise.hz), PP("noise_q", noise.q), PP("noise_t60", noise.t60),
    PP("wire_amount", wires.amount), PP("wire_threshold", wires.threshold), PP("wire_hp_hz", wires.hp_hz),
    PP("wire_lp_hz", wires.lp_hz), PP("wire_ring_ms", wires.ring_ms),
    PP("piece_gain", gain), PP("pan", pan),
    PA("pos", pos), PA("pos_jitter", pos_jitter), PA("contact_ms", contact_ms), PA("contact_exp", contact_exp),
    PA("drive", drive), PA("click", click), PA("click_hz", click_hz), PA("click_q", click_q), PA("click_ms", click_ms),
    PA("art_noise", noise), PA("art_wires", wires), PA("art_cascade", cascade), PA("damp", damp), PA("art_gain", gain),
};
#undef PP
#undef PA
const int kParamCount = int(sizeof(kParams) / sizeof(kParams[0]));

}  // namespace

// ============================================================================================

struct DrumKit::Impl {
  KitTable kit = Kit();  // this kit's own copy: calibration changes it
  std::string cal_name;
  std::vector<Piece> pieces;
  struct Slot { int piece = -1; bool active = false; } voices[Polyphony];
};

DrumKit::DrumKit() : InstrumentProcessor(), impl_(new Impl()), cdata_(nullptr) {}

DrumKit::~DrumKit() {
  Unload();
  delete impl_;
}

int DrumKit::Load() {
  if (loaded_)
    return S_FALSE;
  const KitTable& kit = impl_->kit;
  impl_->pieces.clear();
  impl_->pieces.resize(kit.pieces.size());  // never resized while loaded: banks point into their own memory
  for (size_t i = 0; i < kit.pieces.size(); ++i)
    InitPiece(impl_->pieces[i], kit.pieces[i], double(sample_rate_), uint32_t(0x1234 + 977 * i));
  for (auto& s : impl_->voices) s = Impl::Slot();
  loaded_ = true;
  return S_OK;
}

int DrumKit::Unload() {
  if (!loaded_)
    return S_FALSE;
  for (Piece& p : impl_->pieces) p.bank.Free();
  impl_->pieces.clear();
  loaded_ = false;
  return S_OK;
}

int DrumKit::ParamCount() { return kParamCount; }
const char* DrumKit::ParamName(int i) { return i >= 0 && i < kParamCount ? kParams[i].name : nullptr; }

static float* ParamField(DrumKit::Impl* impl, int note, const char* name, int* piece_out) {
  if (note < kFirstNote || note > kLastNote) return nullptr;
  Articulation& a = impl->kit.notes[note - kFirstNote];
  if (a.piece < 0) return nullptr;
  for (const ParamInfo& p : kParams) {
    if (strcmp(p.name, name)) continue;
    *piece_out = p.piece ? a.piece : -1;
    char* base = p.piece ? reinterpret_cast<char*>(&impl->kit.pieces[a.piece]) : reinterpret_cast<char*>(&a);
    return reinterpret_cast<float*>(base + p.offset);
  }
  return nullptr;
}

bool DrumKit::SetParam(int note, const char* name, float value) {
  int piece = -1;
  float* field = ParamField(impl_, note, name, &piece);
  if (!field) return false;
  *field = value;
  if (piece >= 0 && loaded_) {  // rebuild the piece's modes
    Piece& p = impl_->pieces[piece];
    if (p.owner >= 0) impl_->voices[p.owner].active = false;
    p.bank.Free();
    p = Piece();
    InitPiece(p, impl_->kit.pieces[piece], double(sample_rate_), uint32_t(0x1234 + 977 * piece));
  }
  return true;
}

bool DrumKit::GetParam(int note, const char* name, float* value) const {
  int piece = -1;
  float* field = ParamField(impl_, note, name, &piece);
  if (!field) return false;
  *value = *field;
  return true;
}

bool DrumKit::LoadCalibration(const char* path) {
  FILE* f = nullptr;
  if (fopen_s(&f, path, "r") != 0 || !f) return false;
  char line[512];
  int applied = 0;
  while (fgets(line, sizeof(line), f)) {
    char key[64] = {}, text[256] = {};
    int note = 0;
    float value = 0;
    if (line[0] == '#') continue;
    if (sscanf_s(line, "name %255[^\r\n]", text, unsigned(sizeof(text))) == 1) { impl_->cal_name = text; continue; }
    if (sscanf_s(line, "%d %63s %f", &note, key, unsigned(sizeof(key)), &value) == 3 && SetParam(note, key, value))
      ++applied;
  }
  fclose(f);
  return applied > 0;
}

const std::string& DrumKit::calibration_name() const { return impl_->cal_name; }

void DrumKit::Silence() {
  if (!loaded_) return;
  for (Piece& p : impl_->pieces) {
    if (p.owner >= 0) impl_->voices[p.owner].active = false;
    p.owner = -1;
    p.active = false;
    ResetPiece(p);
    p.seed = p.seed0;
  }
}

const char* DrumKit::DrumName(int note) {
  if (note < kFirstNote || note > kLastNote) return nullptr;
  return Kit().notes[note - kFirstNote].name;
}

const char* DrumKit::PieceName(int note) {
  if (note < kFirstNote || note > kLastNote) return nullptr;
  const Articulation& a = Kit().notes[note - kFirstNote];
  return a.piece >= 0 ? Kit().pieces[a.piece].name : nullptr;
}

int DrumKit::PieceModes(int note) const {
  if (!loaded_ || note < kFirstNote || note > kLastNote) return 0;
  const Articulation& a = impl_->kit.notes[note - kFirstNote];
  return a.piece >= 0 ? impl_->pieces[a.piece].bank.count : 0;
}

int DrumKit::SetFrequency(real_t freq, int v) {
  auto& nd = cdata_->note_data_array[v];
  nd.freq = nd.base_freq = freq;
  return S_OK;
}

int DrumKit::NoteOn(int v) {
  const auto& nd = cdata_->note_data_array[v];
  auto& slot = impl_->voices[v];
  // This voice may still be listed as the owner of a piece it played before.
  if (slot.piece >= 0 && impl_->pieces[slot.piece].owner == v) {
    impl_->pieces[slot.piece].owner = -1;
    impl_->pieces[slot.piece].active = false;
  }
  slot.piece = -1;
  slot.active = false;
  if (nd.note < kFirstNote || nd.note > kLastNote) return S_OK;
  const Articulation& a = impl_->kit.notes[nd.note - kFirstNote];
  if (a.piece < 0) return S_OK;
  Piece& p = impl_->pieces[a.piece];
  // One physical object, one voice: the new hit takes over the piece's vibration.
  if (p.owner >= 0 && p.owner != v) impl_->voices[p.owner].active = false;
  if (!p.active) ResetPiece(p);
  p.owner = v;
  p.active = true;
  slot.piece = a.piece;
  slot.active = true;
  Strike(p, a, float(nd.velocity));
  return S_OK;
}

int DrumKit::NoteOff(int) {
  return S_OK;  // drums ring on; chokes come from the next stroke (hi-hat, mutes)
}

bool DrumKit::IsVoiceActive(int v) const {
  if (!loaded_) return false;
  const auto& slot = impl_->voices[v];
  if (!slot.active) return false;
  const Piece& p = impl_->pieces[slot.piece];
  return p.owner == v && p.active;
}

real_t DrumKit::VoiceLevel(int v) const {
  return IsVoiceActive(v) ? impl_->pieces[impl_->voices[v].piece].level : 0;
}

real_t DrumKit::VoicePan(int v) const {
  if (!loaded_ || impl_->voices[v].piece < 0) return 0.5f;
  return impl_->pieces[impl_->voices[v].piece].def->pan;
}

real_t DrumKit::Tick(int v) {
  auto& slot = impl_->voices[v];
  if (!slot.active) return 0;
  Piece& p = impl_->pieces[slot.piece];
  if (p.owner != v || !p.active) {
    slot.active = false;
    return 0;
  }
  if (p.pos >= kBlock) {
    RenderPiece(p);
    p.pos = 0;
  }
  return p.buf[p.pos++];
}

}
}
}
}
