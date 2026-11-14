// DSP_Render: offline rendering and measurement for the DSP synth.
//
//   DSP_Render render <in.mid> <out.wav> [rate]
//       Render a whole MIDI file (with the master limiter) to a 32-bit float WAV.
//   DSP_Render note <program> <note> <velocity> <hold_s> <out.wav> [tail_s] [rate]
//       Render one note. program is GM 1..128 (0 = drum channel).
//   DSP_Render tune <program> [first_note] [last_note] [step] [rate]
//       Render notes across the range and measure pitch error (cents) and decay times.
//   DSP_Render cpu <program> <voices> [seconds] [rate]
//       Time the render of <voices> simultaneous held notes.
#define NOMINMAX  // use std::min/std::max, not the windows.h macros
#include <algorithm>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <vector>
#include "../DSP_Lib/Code/dsp.h"

using namespace dsp::audio;

namespace {

const uint32_t kBlock = 512;

class OfflineHost : public synth::SynthHost {
 public:
  bool stopped = false;
  void Stop() override { stopped = true; }
};

struct Engine {
  synth::MidiSynth synth;
  OfflineHost host;
  uint32_t rate;
  explicit Engine(uint32_t sample_rate) : rate(sample_rate) {
    synth.player_ = &host;
    synth.set_sample_rate(rate);
    synth.Initialize();
  }
  ~Engine() { synth.Deinitialize(); }
  // Renders frames of idle (live) audio, appending interleaved stereo to out.
  void RenderIdle(uint32_t frames, std::vector<float>& out) {
    float block[kBlock * 2];
    while (frames > 0) {
      uint32_t n = frames < kBlock ? frames : kBlock;
      synth.RenderIdleStereo(n, block);
      out.insert(out.end(), block, block + n * 2);
      frames -= n;
    }
  }
  void SelectProgram(int program) {  // GM 1..128, 0 = drums (channel 10)
    if (program > 0)
      synth.HandleMidiMessage(0xC0, uint8_t(program - 1), 0);
  }
  uint8_t channel_for(int program) const { return program == 0 ? 9 : 0; }
};

bool WriteWav(const char* path, const std::vector<float>& stereo, uint32_t rate) {
  FILE* f = nullptr;
  if (fopen_s(&f, path, "wb") != 0 || !f) {
    fprintf(stderr, "cannot write %s\n", path);
    return false;
  }
  const uint32_t data_bytes = uint32_t(stereo.size() * sizeof(float));
  auto u32 = [&](uint32_t v) { fwrite(&v, 4, 1, f); };
  auto u16 = [&](uint16_t v) { fwrite(&v, 2, 1, f); };
  fwrite("RIFF", 1, 4, f); u32(36 + data_bytes); fwrite("WAVE", 1, 4, f);
  fwrite("fmt ", 1, 4, f); u32(16); u16(3 /*IEEE float*/); u16(2); u32(rate); u32(rate * 8); u16(8); u16(32);
  fwrite("data", 1, 4, f); u32(data_bytes);
  fwrite(stereo.data(), 1, data_bytes, f);
  fclose(f);
  return true;
}

std::vector<float> RenderNote(Engine& e, int program, int note, int velocity, double hold_s, double tail_s) {
  std::vector<float> out;
  e.SelectProgram(program);
  const uint8_t ch = e.channel_for(program);
  e.synth.HandleMidiMessage(0x90 | ch, uint8_t(note), uint8_t(velocity));
  e.RenderIdle(uint32_t(hold_s * e.rate), out);
  e.synth.HandleMidiMessage(0x80 | ch, uint8_t(note), 64);
  e.RenderIdle(uint32_t(tail_s * e.rate), out);
  return out;
}

std::vector<float> ToMono(const std::vector<float>& stereo) {
  std::vector<float> mono(stereo.size() / 2);
  for (size_t i = 0; i < mono.size(); ++i)
    mono[i] = 0.5f * (stereo[i * 2] + stereo[i * 2 + 1]);
  return mono;
}

// Fundamental near `expected_hz`, from the squared-difference function (YIN-style) with
// parabolic refinement. The search is limited to +-~4 semitones so octave errors can't happen.
double MeasurePitch(const std::vector<float>& mono, uint32_t rate, double expected_hz, double start_s) {
  const double period = rate / expected_hz;
  const int min_lag = int(floor(period * 0.8)), max_lag = int(ceil(period * 1.25)) + 1;
  const size_t start = size_t(start_s * rate);
  const size_t window = size_t(std::max(4096.0, period * 8));
  if (start + window + max_lag + 1 >= mono.size() || min_lag < 2)
    return 0;
  auto diff = [&](int lag) {
    double sum = 0;
    for (size_t i = 0; i < window; ++i) {
      double d = mono[start + i] - mono[start + i + lag];
      sum += d * d;
    }
    return sum;
  };
  int best = min_lag;
  double best_value = 1e300;
  for (int lag = min_lag; lag <= max_lag; ++lag) {
    double v = diff(lag);
    if (v < best_value) { best_value = v; best = lag; }
  }
  double a = diff(best - 1), b = best_value, c = diff(best + 1);
  double denom = a - 2 * b + c;
  double offset = denom > 0 ? 0.5 * (a - c) / denom : 0;
  return rate / (best + offset);
}

struct Decay { double peak_db, t20, t40; };

// Times (s) from the loudest 10 ms frame until the level first drops 20 and 40 dB.
Decay MeasureDecay(const std::vector<float>& mono, uint32_t rate, double until_s) {
  const size_t frame = rate / 100;
  const size_t frames = std::min(mono.size(), size_t(until_s * rate)) / frame;
  std::vector<double> db(frames);
  for (size_t f = 0; f < frames; ++f) {
    double sum = 0;
    for (size_t i = 0; i < frame; ++i) sum += double(mono[f * frame + i]) * mono[f * frame + i];
    db[f] = 10 * log10(sum / frame + 1e-20);
  }
  size_t peak = 0;
  for (size_t f = 1; f < frames; ++f) if (db[f] > db[peak]) peak = f;
  Decay d = {frames ? db[peak] : -200, -1, -1};
  for (size_t f = peak; f < frames; ++f) {
    if (d.t20 < 0 && db[f] < db[peak] - 20) d.t20 = (f - peak) * 0.01;
    if (d.t40 < 0 && db[f] < db[peak] - 40) { d.t40 = (f - peak) * 0.01; break; }
  }
  return d;
}

const char* NoteName(int note, char* buf, size_t size) {
  static const char* names[] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
  sprintf_s(buf, size, "%s%d", names[note % 12], note / 12 - 1);
  return buf;
}

int CmdRender(int argc, char** argv) {
  if (argc < 4) return 2;
  uint32_t rate = argc > 4 ? atoi(argv[4]) : 48000;
  Engine e(rate);
  e.synth.LoadMidiFromFile(argv[2]);
  e.host.stopped = false;  // LoadMidi calls Stop() before loading
  const double length_s = e.host.song_length_ms / 1000.0;
  if (length_s <= 0) { fprintf(stderr, "could not load %s\n", argv[2]); return 1; }

  synth::effects::MasterLimiter limiter;
  limiter.Configure(rate);
  std::vector<float> out;
  float block[kBlock * 2];
  float min_gain = 1.0f;
  const uint64_t max_frames = uint64_t((length_s + 30) * rate);
  LARGE_INTEGER t0, t1, freq;
  QueryPerformanceFrequency(&freq);
  QueryPerformanceCounter(&t0);
  while (!e.host.stopped && out.size() / 2 < max_frames) {
    e.synth.RenderSamplesStereo(kBlock, block);
    min_gain = std::min(min_gain, limiter.Process(block, kBlock));
    out.insert(out.end(), block, block + kBlock * 2);
  }
  e.synth.AllNotesOff();
  for (uint32_t i = 0; i < 2 * rate / kBlock; ++i) {  // 2 s release tail
    e.synth.RenderIdleStereo(kBlock, block);
    min_gain = std::min(min_gain, limiter.Process(block, kBlock));
    out.insert(out.end(), block, block + kBlock * 2);
  }
  QueryPerformanceCounter(&t1);
  double cpu_s = double(t1.QuadPart - t0.QuadPart) / freq.QuadPart;
  float peak = 0;
  for (float v : out) peak = std::max(peak, fabsf(v));
  double seconds = out.size() / 2.0 / rate;
  printf("%s: %.1f s audio in %.2f s (%.0fx realtime), peak %.1f dBFS, limiter max %.1f dB\n", argv[3], seconds,
         cpu_s, seconds / cpu_s, 20 * log10(peak + 1e-12), 20 * log10(min_gain));
  return WriteWav(argv[3], out, rate) ? 0 : 1;
}

int CmdNote(int argc, char** argv) {
  if (argc < 7) return 2;
  int program = atoi(argv[2]), note = atoi(argv[3]), velocity = atoi(argv[4]);
  double hold = atof(argv[5]);
  double tail = argc > 7 ? atof(argv[7]) : 2.0;
  uint32_t rate = argc > 8 ? atoi(argv[8]) : 48000;
  Engine e(rate);
  auto out = RenderNote(e, program, note, velocity, hold, tail);
  float peak = 0;
  for (float v : out) peak = std::max(peak, fabsf(v));
  printf("%s: program %d note %d vel %d, peak %.1f dBFS\n", argv[6], program, note, velocity,
         20 * log10(peak + 1e-12));
  return WriteWav(argv[6], out, rate) ? 0 : 1;
}

int CmdTune(int argc, char** argv) {
  if (argc < 3) return 2;
  int program = atoi(argv[2]);
  int first = argc > 3 ? atoi(argv[3]) : 24, last = argc > 4 ? atoi(argv[4]) : 108, step = argc > 5 ? atoi(argv[5]) : 6;
  uint32_t rate = argc > 6 ? atoi(argv[6]) : 48000;
  printf("program %d at %u Hz (note held 4 s)\n", program, rate);
  printf("note   expected Hz  measured Hz   cents   peak dB   -20dB s  -40dB s\n");
  double worst = 0;
  for (int note = first; note <= last; note += step) {
    Engine e(rate);  // fresh engine per note: no leftovers
    auto mono = ToMono(RenderNote(e, program, note, 100, 4.0, 0.2));
    double expected = 440.0 * pow(2.0, (note - 69) / 12.0);
    double measured = MeasurePitch(mono, rate, expected, 0.15);
    double cents = measured > 0 ? 1200 * log2(measured / expected) : 0;
    Decay d = MeasureDecay(mono, rate, 4.0);
    char name[8];
    printf("%-5s  %10.2f  %11.2f  %+7.1f  %8.1f  %8.2f  %7.2f\n", NoteName(note, name, sizeof(name)), expected,
           measured, cents, d.peak_db, d.t20, d.t40);
    if (fabs(cents) > fabs(worst)) worst = cents;
  }
  printf("worst pitch error: %+.1f cents\n", worst);
  return 0;
}

int CmdCpu(int argc, char** argv) {
  if (argc < 4) return 2;
  int program = atoi(argv[2]), voices = atoi(argv[3]);
  double seconds = argc > 4 ? atof(argv[4]) : 10.0;
  uint32_t rate = argc > 5 ? atoi(argv[5]) : 48000;
  Engine e(rate);
  e.SelectProgram(program);
  const uint8_t ch = e.channel_for(program);
  e.synth.HandleMidiMessage(0xB0 | ch, 64, 127);  // sustain so plucked notes keep a voice
  for (int v = 0; v < voices; ++v)  // distinct notes (a repeated note would retrigger, not add a voice)
    e.synth.HandleMidiMessage(0x90 | ch, uint8_t(28 + v % 72), 100);
  float block[kBlock * 2];
  const uint32_t blocks = uint32_t(seconds * rate / kBlock);
  LARGE_INTEGER t0, t1, freq;
  QueryPerformanceFrequency(&freq);
  QueryPerformanceCounter(&t0);
  for (uint32_t b = 0; b < blocks; ++b)
    e.synth.RenderIdleStereo(kBlock, block);
  QueryPerformanceCounter(&t1);
  double cpu_s = double(t1.QuadPart - t0.QuadPart) / freq.QuadPart;
  double audio_s = double(blocks) * kBlock / rate;
  printf("program %d, %d voices: %.2f%% of one core (%.3f%% per voice)\n", program, voices, 100 * cpu_s / audio_s,
         100 * cpu_s / audio_s / std::max(1, voices));
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  int result = 2;
  if (argc >= 2) {
    if (!strcmp(argv[1], "render")) result = CmdRender(argc, argv);
    else if (!strcmp(argv[1], "note")) result = CmdNote(argc, argv);
    else if (!strcmp(argv[1], "tune")) result = CmdTune(argc, argv);
    else if (!strcmp(argv[1], "cpu")) result = CmdCpu(argc, argv);
  }
  if (result == 2) {
    fprintf(stderr,
            "usage:\n"
            "  DSP_Render render <in.mid> <out.wav> [rate]\n"
            "  DSP_Render note <program 1-128|0=drums> <note> <velocity> <hold_s> <out.wav> [tail_s] [rate]\n"
            "  DSP_Render tune <program> [first_note] [last_note] [step] [rate]\n"
            "  DSP_Render cpu <program> <voices> [seconds] [rate]\n");
  }
  return result;
}
