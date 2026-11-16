// DSP_Render: offline rendering and measurement for the DSP synth.
//
//   DSP_Render render <in.mid> <out.wav> [rate] [program]
//       Render a whole MIDI file (with the master limiter) to a 32-bit float WAV.
//   DSP_Render note <program> <note> <velocity> <hold_s> <out.wav> [tail_s] [rate]
//       Render one note. program is GM 1..128 (0 = drum channel).
//   DSP_Render tune <program> [first_note] [last_note] [step] [rate]
//       Render notes across the range and measure pitch error (cents) and decay times.
//   DSP_Render cpu <program> <voices> [seconds] [rate]
//       Time the render of <voices> simultaneous held notes.
//   DSP_Render drums [rate] [wav_dir]
//       Survey every GM drum note: peak level and decay (optionally one WAV each).
#define NOMINMAX  // use std::min/std::max, not the windows.h macros
#include <algorithm>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <vector>
#include "../DSP_Lib/Code/dsp.h"
#include "analysis.h"

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

// Frequency of the strongest spectral peak within +-1 semitone of `expected_hz` (Goertzel scan
// in 1-cent steps over a Hann window, refined with a parabola). Unlike the period method this
// works for inharmonic sounds (bells, bars, drums) where the waveform has no clean period.
double MeasurePeak(const std::vector<float>& mono, uint32_t rate, double expected_hz, double start_s) {
  const size_t start = size_t(start_s * rate);
  const size_t window = 16384;
  if (start + window >= mono.size())
    return 0;
  std::vector<double> x(window);
  for (size_t i = 0; i < window; ++i)
    x[i] = mono[start + i] * (0.5 - 0.5 * cos(2.0 * 3.14159265358979 * i / (window - 1)));
  auto power = [&](double f) {
    double coeff = 2.0 * cos(2.0 * 3.14159265358979 * f / rate), s1 = 0, s2 = 0;
    for (size_t i = 0; i < window; ++i) {
      double s0 = x[i] + coeff * s1 - s2;
      s2 = s1; s1 = s0;
    }
    return s1 * s1 + s2 * s2 - coeff * s1 * s2;
  };
  double best_cents = 0, best = -1, prev = 0, next = 0;
  std::vector<double> p(201);
  for (int c = -100; c <= 100; ++c)
    p[c + 100] = power(expected_hz * pow(2.0, c / 1200.0));
  for (int i = 1; i < 200; ++i)
    if (p[i] > best) { best = p[i]; best_cents = i - 100; prev = p[i - 1]; next = p[i + 1]; }
  double denom = prev - 2 * best + next;
  if (denom < 0) best_cents += 0.5 * (prev - next) / denom;
  return expected_hz * pow(2.0, best_cents / 1200.0);
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
  int program = argc > 5 ? atoi(argv[5]) : 0;  // GM 1..128 to play every melodic part on; 0 = as written
  Engine e(rate);
  if (program > 0) e.synth.set_program_override(program - 1);
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
  uint64_t blocks = 0, limited_blocks = 0;
  while (!e.host.stopped && out.size() / 2 < max_frames) {
    e.synth.RenderSamplesStereo(kBlock, block);
    float g = limiter.Process(block, kBlock);
    min_gain = std::min(min_gain, g);
    ++blocks;
    if (g < 0.891f) ++limited_blocks;  // more than 1 dB of gain reduction
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
  printf("%s: %.1f s audio in %.2f s (%.0fx realtime), peak %.1f dBFS, limiter max %.1f dB, >1 dB for %.1f%% of the song\n",
         argv[3], seconds, cpu_s, seconds / cpu_s, 20 * log10(peak + 1e-12), 20 * log10(min_gain),
         blocks ? 100.0 * limited_blocks / blocks : 0.0);
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
  printf("(period: waveform repeat, meaningful for harmonic sounds; peak: strongest partial near the note)\n");
  printf("note   expected Hz  period Hz  cents   peak Hz    cents   peak dB  -20dB s  -40dB s\n");
  double worst_period = 0, worst_peak = 0;
  for (int note = first; note <= last; note += step) {
    Engine e(rate);  // fresh engine per note: no leftovers
    auto mono = ToMono(RenderNote(e, program, note, 100, 4.0, 0.2));
    double expected = 440.0 * pow(2.0, (note - 69) / 12.0);
    double period_hz = MeasurePitch(mono, rate, expected, 0.15);
    double peak_hz = MeasurePeak(mono, rate, expected, 0.05);
    double c1 = period_hz > 0 ? 1200 * log2(period_hz / expected) : 0;
    double c2 = peak_hz > 0 ? 1200 * log2(peak_hz / expected) : 0;
    Decay d = MeasureDecay(mono, rate, 4.0);
    char name[8];
    printf("%-5s  %10.2f  %9.2f  %+6.1f  %9.2f  %+6.1f  %8.1f  %7.2f  %7.2f\n", NoteName(note, name, sizeof(name)),
           expected, period_hz, c1, peak_hz, c2, d.peak_db, d.t20, d.t40);
    if (fabs(c1) > fabs(worst_period)) worst_period = c1;
    if (fabs(c2) > fabs(worst_peak)) worst_peak = c2;
  }
  printf("worst pitch error: period %+.1f cents, peak %+.1f cents\n", worst_period, worst_peak);
  return 0;
}

int CmdAnalyze(int argc, char** argv) {
  if (argc < 3) return 2;
  analysis::Audio audio;
  std::string error;
  if (!analysis::ReadWav(argv[2], audio, &error)) { fprintf(stderr, "%s: %s\n", argv[2], error.c_str()); return 1; }
  int note = argc > 3 ? atoi(argv[3]) : analysis::NoteFromFileName(argv[2]);
  if (note < 0) { fprintf(stderr, "no note in the file name; pass it as the next argument\n"); return 1; }
  analysis::NoteAnalysis a;
  if (!analysis::AnalyzeNote(audio, note, a)) { fprintf(stderr, "analysis failed\n"); return 1; }
  printf("%s (%u Hz, %d ch, %.2f s)\n", argv[2], audio.rate, audio.channels, audio.mono.size() / double(audio.rate));
  analysis::PrintAnalysis(a);
  return 0;
}

// WAV files in a folder with the note named in each file name, sorted by note.
std::vector<std::pair<int, std::string>> CollectRecordings(const char* folder) {
  std::vector<std::pair<int, std::string>> files;
  WIN32_FIND_DATAA fd;
  std::string pattern = std::string(folder) + "\\*.wav";
  HANDLE h = FindFirstFileA(pattern.c_str(), &fd);
  if (h == INVALID_HANDLE_VALUE) return files;
  do {
    std::string path = std::string(folder) + "\\" + fd.cFileName;
    int note = analysis::NoteFromFileName(path.c_str());
    if (note >= 0) files.push_back({note, path});
  } while (FindNextFileA(h, &fd));
  FindClose(h);
  std::sort(files.begin(), files.end());
  return files;
}

// A library names all its files one way, so use the most common octave correction for all.
int ConsensusOctave(const std::vector<std::pair<int, std::string>>& files) {
  int votes[3] = {0, 0, 0};
  for (auto& [named, path] : files) {
    analysis::Audio audio;
    analysis::NoteAnalysis a;
    if (analysis::ReadWav(path.c_str(), audio) && analysis::AnalyzeNote(audio, named, a))
      ++votes[a.octave_shift / 12 + 1];
  }
  int shift = (votes[0] > votes[1] && votes[0] > votes[2]) ? -12 : (votes[2] > votes[1] ? 12 : 0);
  printf("octave correction: %+d semitones (votes: -12:%d  0:%d  +12:%d)\n", shift, votes[0], votes[1], votes[2]);
  return shift;
}

// Per-partial decays of a recorded note, for the instrument's resonator band. Implausible fits
// (very short or longer than any piano partial rings) are left unmeasured; an aftersound counts
// only when it is clearly slower than the first stage and neither buried nor dominant.
std::vector<synth::instruments::PartialDecay> PartialDecays(const analysis::NoteAnalysis& a, int max_partials = 24) {
  std::vector<synth::instruments::PartialDecay> out(max_partials);
  for (const analysis::Partial& p : a.partials) {
    if (p.n < 1 || p.n > max_partials || p.t60_s <= 0) continue;
    synth::instruments::PartialDecay& d = out[p.n - 1];
    const bool two_stage = p.after_db > -45 && p.after_db < -1 && p.after_t60_s > 1.2 * p.prompt_t60_s;
    const double first = two_stage ? p.prompt_t60_s : p.t60_s;
    if (first < 0.1 || first > 150) continue;
    d.t60_s = float(first);
    if (two_stage) {
      d.after_t60_s = float(std::min(150.0, p.after_t60_s));
      d.after_level = float(pow(10.0, p.after_db / 20.0));
    }
  }
  while (!out.empty() && out.back().t60_s == 0) out.pop_back();
  return out;
}

// Analyses every WAV in a folder and prints one summary line per note.
int CmdSurvey(int argc, char** argv) {
  if (argc < 3) return 2;
  auto files = CollectRecordings(argv[2]);
  if (files.empty()) { fprintf(stderr, "no WAV files with note names in %s\n", argv[2]); return 1; }
  int shift = ConsensusOctave(files);
  printf("note  vel MIDI    f0 Hz   cents        B   parts  T60(f0)  T60 p2  T60 p4  T60@2k  T60@5k  peak dB | 2-stage: after dB  knee s  len s\n");
  for (auto& [named, path] : files) {
    if (named < 0) continue;
    analysis::Audio audio;
    analysis::NoteAnalysis a;
    if (!analysis::ReadWav(path.c_str(), audio) || !analysis::AnalyzeNote(audio, named, a, 40, shift)) {
      printf("%s: analysis failed\n", path.c_str());
      continue;
    }
    auto t60_of = [&](int n) {
      for (auto& p : a.partials) if (p.n == n) return p.t60_s;
      return -1.0;
    };
    auto model = [&](double f) { return 6.907755 / std::max(1e-9, a.b1 + a.b3 * f * f); };
    int two_stage = 0;
    for (auto& p : a.partials) if (p.n <= 12 && p.after_db > -99) ++two_stage;
    char name[8], stage[48];
    if (a.after_db > -99) sprintf_s(stage, "%+6.1f  %5.2f (%d)", a.after_db, a.knee_s, two_stage);
    else sprintf_s(stage, "     -      -     ");
    printf("%-5s %3d %4d  %8.2f  %+5.1f  %8.1e  %4d   %6.2f  %6.2f  %6.2f  %6.2f  %6.2f  %6.1f   | %s  %5.1f\n",
           NoteName(a.midi_note, name, sizeof(name)), analysis::VelocityLayerFromFileName(path.c_str()), a.midi_note,
           a.f0_hz, a.tuning_cents, a.inharmonicity, int(a.partials.size()), t60_of(1), t60_of(2), t60_of(4),
           model(2000), model(5000), 20 * log10(a.peak + 1e-12), stage, audio.mono.size() / double(audio.rate));
  }
  return 0;
}

// Builds a calibration file from a folder of recorded notes: measured string parameters per note
// plus the excitation (pluck + body) obtained by inverse-filtering each recording with the fitted
// string loop.
int CmdCalibrate(int argc, char** argv) {
  if (argc < 4) return 2;
  const double exc_ms = argc > 4 ? atof(argv[4]) : 200.0;
  auto files = CollectRecordings(argv[2]);
  if (files.empty()) { fprintf(stderr, "no WAV files with note names in %s\n", argv[2]); return 1; }
  int shift = ConsensusOctave(files);
  using namespace synth::instruments;
  StringCalibration cal;
  // Name: the output file's stem (e.g. "harpsichord_english").
  std::string stem = argv[3];
  size_t slash = stem.find_last_of("\\/");
  if (slash != std::string::npos) stem = stem.substr(slash + 1);
  size_t dot = stem.find_last_of('.');
  if (dot != std::string::npos) stem.resize(dot);
  cal.name = stem;
  // Group recordings by note; each file of a note is one velocity layer ("_vl3_", "_v2_"...).
  std::vector<std::pair<int, std::vector<std::pair<int, std::string>>>> groups;
  int max_layer = 1;
  for (auto& [named, path] : files) {
    int layer = std::max(1, analysis::VelocityLayerFromFileName(path.c_str()));
    max_layer = std::max(max_layer, layer);
    if (groups.empty() || groups.back().first != named) groups.push_back({named, {}});
    groups.back().second.push_back({layer, path});
  }
  printf("note  layers    f0 Hz        B   T60 f0  T60 2k  disp rms/max, filter | per layer: velocity, string removed, exc peak, two-stage/measured partials\n");
  for (auto& [named, layers] : groups) {
    std::sort(layers.begin(), layers.end());
    // String parameters from the loudest layer (most partials above the noise).
    analysis::Audio audio;
    analysis::NoteAnalysis a;
    if (!analysis::ReadWav(layers.back().second.c_str(), audio) || !analysis::AnalyzeNote(audio, named, a, 40, shift)) {
      printf("%s: analysis failed\n", layers.back().second.c_str());
      continue;
    }
    LoopParams lp = {a.f0_hz, a.inharmonicity, std::max(0.01, a.b1), std::max(0.0, a.b3)};
    LoopDesign design = DesignLoop(audio.rate, lp, DispersionStagesFor(a.f0_hz));

    CalibratedNote cn;
    cn.midi_note = a.midi_note;
    cn.f0_hz = float(a.f0_hz);
    // Too few partials to see the stretch: leave B unknown (0) so the instrument extrapolates it.
    cn.inharmonicity = a.partials.size() >= 5 ? float(a.inharmonicity) : 0.0f;
    cn.b1 = float(lp.b1);
    cn.b3 = float(lp.b3);
    cn.peak = float(a.peak);
    cn.excitation_rate = audio.rate;
    // The two-stage decay (aftersound) is stored per partial with each layer below; the instrument
    // plays the low partials as resonators, so the older second-string model stays off.

    char name[8];
    auto t60 = [&](double f) { return 6.907755 / (lp.b1 + lp.b3 * f * f); };
    char disp[24];
    if (design.biquads) sprintf_s(disp, "AS%2d", design.biquads);
    else sprintf_s(disp, "1o%2d", design.stages);
    printf("%-5s %4d  %8.2f  %8.1e  %6.2f  %6.2f  %5.1f/%5.1f c %s |", NoteName(a.midi_note, name, sizeof(name)),
           int(layers.size()), a.f0_hz, a.inharmonicity, t60(a.f0_hz), t60(2000), design.rms_partial_error_cents,
           design.max_partial_error_cents, disp);
    for (auto& [layer, path] : layers) {
      analysis::Audio la;
      analysis::NoteAnalysis lna;
      if (!analysis::ReadWav(path.c_str(), la) || !analysis::AnalyzeNote(la, named, lna, 40, shift)) continue;
      StringLoop loop;
      loop.Allocate(int(la.rate / 8) + 64);
      loop.SetDesign(design);
      // Inverse filter from just before the onset.
      const size_t start = lna.onset > la.rate / 100 ? lna.onset - la.rate / 100 : 0;
      const size_t exc_len = size_t(exc_ms * 0.001 * la.rate);
      const size_t check_len = std::min(la.mono.size() - start, exc_len + size_t(0.4 * la.rate));
      std::vector<float> residual(check_len);
      for (size_t i = 0; i < check_len; ++i) residual[i] = loop.Inverse(la.mono[start + i]);
      // How much of the recording the fitted string explains after the excitation window.
      double ey = 1e-30, ee = 1e-30;
      for (size_t i = exc_len; i < check_len; ++i) {
        ey += double(la.mono[start + i]) * la.mono[start + i];
        ee += double(residual[i]) * residual[i];
      }
      ExcitationLayer el;
      el.velocity = float(layer) / max_layer;
      el.peak = float(lna.peak);
      el.partials = PartialDecays(lna);
      el.samples.assign(residual.begin(), residual.begin() + std::min(exc_len, residual.size()));
      const size_t fade = el.samples.size() / 4;  // half-Hann fade over the last quarter
      for (size_t i = 0; i < fade; ++i) {
        double w = 0.5 + 0.5 * cos(3.14159265358979 * (i + 1) / fade);
        el.samples[el.samples.size() - fade + i] *= float(w);
      }
      float exc_peak = 0;
      for (float v : el.samples) exc_peak = std::max(exc_peak, fabsf(v));
      int two_stage = 0;
      for (auto& d : el.partials) two_stage += d.after_level > 0;
      printf("  %.2f %4.1f dB %5.1f dB %2d/%-2d", el.velocity, 10 * log10(ey / ee), 20 * log10(exc_peak / std::max(1e-9f, el.peak)),
             two_stage, int(el.partials.size()));
      cn.layers.push_back(std::move(el));
    }
    printf("\n");
    // Single-layer instruments keep the plain excitation field (version 1 behaviour).
    if (cn.layers.size() == 1) {
      cn.excitation = std::move(cn.layers[0].samples);
      cn.partials = std::move(cn.layers[0].partials);
      cn.layers.clear();
    }
    cal.notes.push_back(std::move(cn));
  }
  if (!cal.Save(argv[3])) { fprintf(stderr, "cannot write %s\n", argv[3]); return 1; }
  size_t samples = 0;
  for (auto& n : cal.notes) {
    samples += n.excitation.size();
    for (auto& l : n.layers) samples += l.samples.size();
  }
  printf("wrote %s: %zu notes, %.0f KB of excitation\n", argv[3], cal.notes.size(), samples * 4 / 1024.0);
  return 0;
}

// Renders the note of a recording with the synth (same program, rate and duration), analyses both
// the same way and prints them side by side; optionally writes an A/B file (recording, gap, synth).
int CmdCompare(int argc, char** argv) {
  if (argc < 4) return 2;
  const int program = atoi(argv[2]);
  const char* wav = argv[3];
  const char* ab_path = argc > 4 && strcmp(argv[4], "-") ? argv[4] : nullptr;
  const int velocity = argc > 5 ? atoi(argv[5]) : 100;
  analysis::Audio rec;
  std::string error;
  if (!analysis::ReadWav(wav, rec, &error)) { fprintf(stderr, "%s: %s\n", wav, error.c_str()); return 1; }
  int named = analysis::NoteFromFileName(wav);
  analysis::NoteAnalysis ra;
  if (named < 0 || !analysis::AnalyzeNote(rec, named, ra)) { fprintf(stderr, "cannot analyse %s\n", wav); return 1; }

  Engine e(rec.rate);
  if (e.synth.calibrated_instruments().empty()) printf("note: no calibration found (looked in '%s')\n", e.synth.calibration_dir().c_str());
  const double seconds = rec.mono.size() / double(rec.rate);
  auto stereo = RenderNote(e, program, ra.midi_note, velocity, std::max(0.5, seconds - 0.5), 0.5);
  analysis::Audio syn;
  syn.rate = rec.rate;
  syn.channels = 1;
  syn.mono = ToMono(stereo);
  analysis::NoteAnalysis sa;
  if (!analysis::AnalyzeNote(syn, ra.midi_note, sa, 40, 0)) { fprintf(stderr, "cannot analyse the synth output\n"); return 1; }

  // Levels relative to each sound's strongest partial (absolute gain differs by design).
  auto strongest = [](const analysis::NoteAnalysis& a, double analysis::Partial::*field) {
    double m = -300;
    for (auto& p : a.partials) m = std::max(m, p.*field);
    return m;
  };
  const double rtop = strongest(ra, &analysis::Partial::level_db), stop = strongest(sa, &analysis::Partial::level_db);
  const double rspec = strongest(ra, &analysis::Partial::spec_db), sspec = strongest(sa, &analysis::Partial::spec_db);
  char name[8];
  printf("%s, MIDI %d (%s), program %d at %u Hz\n", wav, ra.midi_note, NoteName(ra.midi_note, name, sizeof(name)), program, rec.rate);
  printf("inharmonicity B: recording %.2e, synth %.2e\n", ra.inharmonicity, sa.inharmonicity);
  printf("(decay fit: level where each partial's decay line starts; spectrum: level 30-770 ms after the onset)\n");
  printf("   n  decay fit: rec  syn  diff | spectrum: rec  syn  diff | rec T60  syn T60  ratio | offset rec / syn (cents)\n");
  double level_err = 0, spec_err = 0, t60_log_err = 0;
  int level_count = 0, t60_count = 0;
  for (auto& rp : ra.partials) {
    const analysis::Partial* sp = nullptr;
    for (auto& p : sa.partials) if (p.n == rp.n) sp = &p;
    if (!sp) { printf("  %2d  %14.1f     -        | %13.1f     -        |  %6.2f       -        |  %+5.1f /   -\n", rp.n,
                      rp.level_db - rtop, rp.spec_db - rspec, rp.t60_s, rp.cents_off); continue; }
    double dl = (sp->level_db - stop) - (rp.level_db - rtop);
    double ds = (sp->spec_db - sspec) - (rp.spec_db - rspec);
    level_err += fabs(dl); spec_err += fabs(ds); ++level_count;
    double ratio = (rp.t60_s > 0 && sp->t60_s > 0) ? sp->t60_s / rp.t60_s : 0;
    if (ratio > 0) { t60_log_err += fabs(log2(ratio)); ++t60_count; }
    printf("  %2d  %14.1f %5.1f %+5.1f | %13.1f %5.1f %+5.1f |  %6.2f   %6.2f  %5.2f |  %+5.1f / %+5.1f\n", rp.n,
           rp.level_db - rtop, sp->level_db - stop, dl, rp.spec_db - rspec, sp->spec_db - sspec, ds, rp.t60_s, sp->t60_s,
           ratio, rp.cents_off, sp->cents_off);
  }
  printf("mean |level difference| %.1f dB (spectrum %.1f dB) over %d partials; T60 typically off by a factor %.2f over %d partials\n",
         level_count ? level_err / level_count : 0.0, level_count ? spec_err / level_count : 0.0, level_count,
         t60_count ? pow(2.0, t60_log_err / t60_count) : 0.0, t60_count);

  if (ab_path) {
    auto peak_of = [](const std::vector<float>& v) { float p = 1e-9f; for (float x : v) p = std::max(p, fabsf(x)); return p; };
    const float target = 0.5f;
    const float gr = target / peak_of(rec.mono), gs = target / peak_of(syn.mono);
    std::vector<float> ab;
    for (float v : rec.mono) { ab.push_back(v * gr); ab.push_back(v * gr); }
    ab.insert(ab.end(), size_t(rec.rate) * 2 * 7 / 10, 0.0f);
    for (float v : syn.mono) { ab.push_back(v * gs); ab.push_back(v * gs); }
    if (WriteWav(ab_path, ab, rec.rate)) printf("A/B written to %s (recording, then synth, both peak-normalised)\n", ab_path);
  }
  return 0;
}

// Designs one string loop and prints the result (for checking the dispersion design).
int CmdDesign(int argc, char** argv) {
  if (argc < 4) return 2;
  using namespace synth::instruments;
  LoopParams p = {atof(argv[2]), atof(argv[3]), 0.5, 1e-7};
  double rate = argc > 4 ? atof(argv[4]) : 44100;
  LoopDesign d = DesignLoop(rate, p, kMaxDispersionStages);
  printf("f0 %.2f Hz, B %.2e at %.0f Hz: delay %d, first-order stages %d, biquads %d, partial error rms %.2f / max %.2f cents\n",
         p.f0_hz, p.inharmonicity, rate, d.delay, d.stages, d.biquads, d.rms_partial_error_cents, d.max_partial_error_cents);
  return 0;
}

int CmdDrums(int argc, char** argv) {
  uint32_t rate = argc > 2 ? atoi(argv[2]) : 48000;
  const char* wav_dir = argc > 3 && strcmp(argv[3], "-") ? argv[3] : nullptr;  // optional: one WAV per drum
  const int velocity = argc > 4 ? atoi(argv[4]) : 100;
  printf("GM drum kit at %u Hz, velocity %d\n", rate, velocity);
  printf("(loud: loudest 50 ms RMS in dBFS; pan: right-left amplitude balance)\n");
  printf("note  name                    peak dBFS  loud dB   -20dB s  -40dB s    pan\n");
  const int first = argc > 5 ? atoi(argv[5]) : 27, last = argc > 6 ? atoi(argv[6]) : (argc > 5 ? first : 87);
  for (int note = first; note <= last; ++note) {
    const char* name = synth::instruments::DrumKit::DrumName(note);
    if (!name) continue;
    Engine e(rate);
    auto stereo = RenderNote(e, 0, note, velocity, 0.1, 2.9);
    auto mono = ToMono(stereo);
    float peak = 0;
    for (float v : mono) peak = std::max(peak, fabsf(v));
    Decay d = MeasureDecay(mono, rate, 3.0);
    double loud = 0, el = 1e-30, er = 1e-30;
    const size_t win = rate / 20;
    for (size_t s = 0; s + win <= mono.size(); s += win / 4) {
      double sum = 0;
      for (size_t i = s; i < s + win; ++i) sum += double(mono[i]) * mono[i];
      loud = std::max(loud, sum / win);
    }
    for (size_t i = 0; i < mono.size(); ++i) {
      el += double(stereo[2 * i]) * stereo[2 * i];
      er += double(stereo[2 * i + 1]) * stereo[2 * i + 1];
    }
    printf("%4d  %-22s  %8.1f  %7.1f  %8.2f  %7.2f  %+5.2f\n", note, name, 20 * log10(peak + 1e-12), 10 * log10(loud + 1e-20),
           d.t20, d.t40, (sqrt(er) - sqrt(el)) / (sqrt(er) + sqrt(el)));
    if (wav_dir) {
      char path[MAX_PATH];
      sprintf_s(path, "%s\\drum_%02d.wav", wav_dir, note);
      WriteWav(path, stereo, rate);
    }
  }
  return 0;
}

// ---- Drum analysis: band energies and spectral centroid over time after the onset ------------

void Fft(std::vector<double>& re, std::vector<double>& im) {  // in place, size a power of 2
  const size_t n = re.size();
  for (size_t i = 1, j = 0; i < n; ++i) {
    size_t bit = n >> 1;
    for (; j & bit; bit >>= 1) j ^= bit;
    j ^= bit;
    if (i < j) { std::swap(re[i], re[j]); std::swap(im[i], im[j]); }
  }
  for (size_t len = 2; len <= n; len <<= 1) {
    const double a = -2 * 3.14159265358979 / len;
    for (size_t i = 0; i < n; i += len)
      for (size_t k = 0; k < len / 2; ++k) {
        const double c = cos(a * k), s = sin(a * k);
        const double xr = re[i + k + len / 2] * c - im[i + k + len / 2] * s;
        const double xi = re[i + k + len / 2] * s + im[i + k + len / 2] * c;
        re[i + k + len / 2] = re[i + k] - xr; im[i + k + len / 2] = im[i + k] - xi;
        re[i + k] += xr; im[i + k] += xi;
      }
  }
}

const int kDrumBands = 5, kDrumWindows = 5;
const double kDrumBandEdges[kDrumBands + 1] = {20, 150, 600, 2500, 7000, 20000};
const double kDrumWindowEdges[kDrumWindows + 1] = {0, 0.01, 0.04, 0.12, 0.35, 1.0};  // s after the onset

struct DrumProfile {
  double peak_db = -200, loud_db = -200, t20 = -1, t40 = -1;
  double band_db[kDrumWindows][kDrumBands];  // energy per band per window, dB re the loudest cell
  double centroid[kDrumWindows];             // Hz
  double low_hz[3];                          // strongest component below 400 Hz at 0, 50 and 150 ms
};

// Strongest spectral peak between lo and hi Hz in a Hann window starting at `start`.
double LowPeak(const std::vector<float>& x, uint32_t rate, size_t start, double lo, double hi) {
  const size_t window = rate / 20;
  if (start + window > x.size()) return 0;
  double best = 0, best_f = 0;
  for (double f = lo; f <= hi; f *= 1.002) {
    double coeff = 2 * cos(2 * 3.14159265358979 * f / rate), s1 = 0, s2 = 0;
    for (size_t i = 0; i < window; ++i) {
      double w = 0.5 - 0.5 * cos(2 * 3.14159265358979 * i / (window - 1));
      double s0 = x[start + i] * w + coeff * s1 - s2;
      s2 = s1; s1 = s0;
    }
    double p = s1 * s1 + s2 * s2 - coeff * s1 * s2;
    if (p > best) { best = p; best_f = f; }
  }
  return best_f;
}

DrumProfile ProfileDrum(const std::vector<float>& mono, uint32_t rate) {
  DrumProfile p;
  float peak = 0;
  for (float v : mono) peak = std::max(peak, fabsf(v));
  p.peak_db = 20 * log10(peak + 1e-12);
  size_t onset = 0;
  while (onset < mono.size() && fabsf(mono[onset]) < 0.05f * peak) ++onset;
  onset = onset > rate / 1000 ? onset - rate / 1000 : 0;
  std::vector<float> x(mono.begin() + onset, mono.end());
  const size_t win = rate / 20;
  double loud = 0;
  for (size_t s = 0; s + win <= x.size(); s += win / 4) {
    double sum = 0;
    for (size_t i = s; i < s + win; ++i) sum += double(x[i]) * x[i];
    loud = std::max(loud, sum / win);
  }
  p.loud_db = 10 * log10(loud + 1e-20);
  Decay d = MeasureDecay(x, rate, x.size() / double(rate));
  p.t20 = d.t20; p.t40 = d.t40;
  double top = 1e-30;
  double energy[kDrumWindows][kDrumBands] = {};
  for (int w = 0; w < kDrumWindows; ++w) {
    const size_t a = size_t(kDrumWindowEdges[w] * rate), b = std::min(x.size(), size_t(kDrumWindowEdges[w + 1] * rate));
    size_t n = 1024;
    while (n < b - a) n <<= 1;
    std::vector<double> re(n, 0.0), im(n, 0.0);
    for (size_t i = a; i < b; ++i) re[i - a] = x[i];  // rectangular: the windows tile the sound
    Fft(re, im);
    double sum = 1e-30, fsum = 0;
    for (size_t k = 1; k < n / 2; ++k) {
      const double f = double(k) * rate / n, pw = re[k] * re[k] + im[k] * im[k];
      sum += pw; fsum += pw * f;
      for (int band = 0; band < kDrumBands; ++band)
        if (f >= kDrumBandEdges[band] && f < kDrumBandEdges[band + 1]) energy[w][band] += pw;
    }
    p.centroid[w] = fsum / sum;
    for (int band = 0; band < kDrumBands; ++band) {
      energy[w][band] /= double(n) * std::max<size_t>(1, b - a);  // power (Parseval), so windows compare
      top = std::max(top, energy[w][band]);
    }
  }
  for (int w = 0; w < kDrumWindows; ++w)
    for (int band = 0; band < kDrumBands; ++band) p.band_db[w][band] = 10 * log10(energy[w][band] / top + 1e-12);
  p.low_hz[0] = LowPeak(x, rate, 0, 30, 400);
  p.low_hz[1] = LowPeak(x, rate, size_t(0.05 * rate), 30, 400);
  p.low_hz[2] = LowPeak(x, rate, size_t(0.15 * rate), 30, 400);
  return p;
}

void PrintProfiles(const DrumProfile* a, const char* a_name, const DrumProfile* b, const char* b_name) {
  printf("            %-34s", a_name);
  if (b) printf(" | %s", b_name);
  printf("\npeak / loudest 50 ms   %6.1f / %6.1f dBFS", a->peak_db, a->loud_db);
  if (b) printf("       | %6.1f / %6.1f dBFS", b->peak_db, b->loud_db);
  printf("\n-20 / -40 dB after     %6.2f / %6.2f s   ", a->t20, a->t40);
  if (b) printf("       | %6.2f / %6.2f s", b->t20, b->t40);
  printf("\nlow peak 0/50/150 ms %5.0f/%5.0f/%5.0f Hz", a->low_hz[0], a->low_hz[1], a->low_hz[2]);
  if (b) printf("       | %5.0f/%5.0f/%5.0f Hz", b->low_hz[0], b->low_hz[1], b->low_hz[2]);
  printf("\nband energy (dB re loudest cell) and spectral centroid per window after the onset:\n");
  printf("window ms   <150 150-600 0.6-2.5k 2.5-7k  >7k  centroid");
  if (b) printf(" |  <150 150-600 0.6-2.5k 2.5-7k  >7k  centroid");
  printf("\n");
  for (int w = 0; w < kDrumWindows; ++w) {
    printf("%4.0f-%-4.0f ", kDrumWindowEdges[w] * 1000, kDrumWindowEdges[w + 1] * 1000);
    for (int band = 0; band < kDrumBands; ++band) printf(" %6.1f", a->band_db[w][band]);
    printf("  %6.0f", a->centroid[w]);
    if (b) {
      printf(" | ");
      for (int band = 0; band < kDrumBands; ++band) printf(" %6.1f", b->band_db[w][band]);
      printf("  %6.0f", b->centroid[w]);
    }
    printf("\n");
  }
}

// Profile of one drum note: kit drum <note> [velocity] [rate]
int CmdDrumInfo(int argc, char** argv) {
  if (argc < 3) return 2;
  const int note = atoi(argv[2]), velocity = argc > 3 ? atoi(argv[3]) : 100;
  const uint32_t rate = argc > 4 ? atoi(argv[4]) : 48000;
  Engine e(rate);
  auto mono = ToMono(RenderNote(e, 0, note, velocity, 0.1, 2.9));
  DrumProfile p = ProfileDrum(mono, rate);
  char title[96];
  const char* name = synth::instruments::DrumKit::DrumName(note);
  sprintf_s(title, "note %d %s, velocity %d", note, name ? name : "?", velocity);
  PrintProfiles(&p, title, nullptr, nullptr);
  printf("envelope (RMS dBFS per 20 ms):");
  for (size_t s = 0, i = 0; s + rate / 50 <= mono.size() && i < 40; s += rate / 50, ++i) {
    double sum = 1e-20;
    for (size_t k = s; k < s + rate / 50; ++k) sum += double(mono[k]) * mono[k];
    printf("%s%.0f", i % 10 ? " " : "\n  ", 10 * log10(sum / (rate / 50)));
  }
  printf("\n");
  return 0;
}

// Renders a drum note like a recording (same rate and length), profiles both the same way and
// prints them side by side; optionally writes an A/B file (recording, gap, synth).
int CmdDrumCompare(int argc, char** argv) {
  if (argc < 4) return 2;
  const int note = atoi(argv[2]);
  const char* wav = argv[3];
  const char* ab_path = argc > 4 && strcmp(argv[4], "-") ? argv[4] : nullptr;
  const int velocity = argc > 5 ? atoi(argv[5]) : 100;
  analysis::Audio rec;
  std::string error;
  if (!analysis::ReadWav(wav, rec, &error)) { fprintf(stderr, "%s: %s\n", wav, error.c_str()); return 1; }
  Engine e(rec.rate);
  const double seconds = std::max(1.0, rec.mono.size() / double(rec.rate));
  auto syn = ToMono(RenderNote(e, 0, note, velocity, 0.05, seconds - 0.05));
  DrumProfile pr = ProfileDrum(rec.mono, rec.rate), ps = ProfileDrum(syn, rec.rate);
  char title[96];
  const char* name = synth::instruments::DrumKit::DrumName(note);
  sprintf_s(title, "synth: note %d %s, velocity %d", note, name ? name : "?", velocity);
  PrintProfiles(&pr, "recording", &ps, title);
  double err = 0;
  for (int w = 0; w < kDrumWindows; ++w)
    for (int band = 0; band < kDrumBands; ++band) err += fabs(std::max(-60.0, pr.band_db[w][band]) - std::max(-60.0, ps.band_db[w][band]));
  printf("mean |band difference| %.1f dB over %d cells\n", err / (kDrumWindows * kDrumBands), kDrumWindows * kDrumBands);
  if (ab_path) {
    auto peak_of = [](const std::vector<float>& v) { float p = 1e-9f; for (float x : v) p = std::max(p, fabsf(x)); return p; };
    const float gr = 0.5f / peak_of(rec.mono), gs = 0.5f / peak_of(syn);
    std::vector<float> ab;
    for (float v : rec.mono) { ab.push_back(v * gr); ab.push_back(v * gr); }
    ab.insert(ab.end(), size_t(rec.rate) * 2 * 7 / 10, 0.0f);
    for (float v : syn) { ab.push_back(v * gs); ab.push_back(v * gs); }
    if (WriteWav(ab_path, ab, rec.rate)) printf("A/B written to %s (recording, then synth, both peak-normalised)\n", ab_path);
  }
  return 0;
}

// What MidiSynth loaded: the calibration folder it found (searched upward from this exe, the
// same way any app in this build folder searches) and the calibrated instruments in use.
int CmdInfo(int argc, char** argv) {
  const uint32_t rate = argc > 2 ? atoi(argv[2]) : 48000;
  Engine e(rate);
  const std::string& dir = e.synth.calibration_dir();
  printf("calibration folder: %s\n", dir.empty() ? "(none found: hand-set presets only)" : dir.c_str());
  if (e.synth.calibrated_instruments().empty()) printf("calibrated instruments: none\n");
  for (const std::string& name : e.synth.calibrated_instruments()) printf("calibrated: %s\n", name.c_str());
  return 0;
}

// ---- Fitting kit parameters to recordings ----------------------------------------------------

std::vector<float> RenderKitNote(synth::instruments::DrumKit& kit, int note, int velocity, size_t frames) {
  kit.Silence();
  uint32_t generation;
  const int v = kit.AllocateVoice(generation);
  kit.StartVoice(v, note, 440, velocity / 127.0f);
  kit.ReleaseVoice(v);
  std::vector<float> out(frames);
  for (float& s : out) s = kit.Tick(v);
  return out;
}

struct FitTarget {
  std::string path;
  int velocity;
  std::vector<float> mono;
  DrumProfile profile;
};

// Distance between two drum profiles: band energies over time (each relative to its own
// loudest cell), decay times and, for pitched membranes, the pitch and its glide.
double ProfileError(const DrumProfile& r, const DrumProfile& s, bool pitched) {
  double e = 0;
  const double window_weight[kDrumWindows] = {1.0, 1.0, 1.0, 0.8, 0.5};
  for (int w = 0; w < kDrumWindows; ++w)
    for (int b = 0; b < kDrumBands; ++b)
      e += window_weight[w] * fabs(std::max(-60.0, r.band_db[w][b]) - std::max(-60.0, s.band_db[w][b]));
  e /= kDrumWindows * kDrumBands;
  auto log_ratio = [](double a, double b) { return a > 0 && b > 0 ? fabs(log2(a / b)) : 1.0; };
  e += 6 * log_ratio(r.t20, s.t20);
  if (r.t40 > 0) e += 4 * log_ratio(r.t40, s.t40);
  if (pitched) {
    e += 24 * log_ratio(r.low_hz[0], s.low_hz[0]);
    if (r.low_hz[2] > 0 && s.low_hz[2] > 0) e += 24 * log_ratio(r.low_hz[0] / r.low_hz[2], s.low_hz[0] / s.low_hz[2]);
  }
  return e;
}

// drumfit <cal file> <note> <param,param,...> <recording.wav>@<velocity> ... [-rounds N]
// Coordinate descent (log steps) on the named parameters of the note's articulation / piece so
// that its profile matches the recordings; writes the fitted values into the calibration file
// (other lines are kept, so notes can be fitted one after another).
int CmdDrumFit(int argc, char** argv) {
  if (argc < 6) return 2;
  const char* cal_path = argv[2];
  const int note = atoi(argv[3]);
  std::vector<std::string> params;
  for (char* p = argv[4]; *p;) {
    char* comma = strchr(p, ',');
    params.push_back(comma ? std::string(p, comma) : std::string(p));
    if (!comma) break;
    p = comma + 1;
  }
  int rounds = 8;
  bool use_pitch = true;  // -nopitch: recordings whose low end is too noisy for a pitch estimate
  std::vector<FitTarget> targets;
  for (int i = 5; i < argc; ++i) {
    if (!strcmp(argv[i], "-rounds") && i + 1 < argc) { rounds = atoi(argv[++i]); continue; }
    if (!strcmp(argv[i], "-nopitch")) { use_pitch = false; continue; }
    std::string arg = argv[i];
    size_t at = arg.rfind('@');
    FitTarget t;
    t.path = arg.substr(0, at);
    t.velocity = at == std::string::npos ? 100 : atoi(arg.c_str() + at + 1);
    analysis::Audio audio;
    std::string error;
    if (!analysis::ReadWav(t.path.c_str(), audio, &error)) { fprintf(stderr, "%s: %s\n", t.path.c_str(), error.c_str()); return 1; }
    // Start just before the hit and keep at most 1.5 s.
    float peak = 0;
    for (float v : audio.mono) peak = std::max(peak, fabsf(v));
    size_t onset = 0;
    while (onset < audio.mono.size() && fabsf(audio.mono[onset]) < 0.05f * peak) ++onset;
    onset = onset > audio.rate / 200 ? onset - audio.rate / 200 : 0;
    const size_t len = std::min(audio.mono.size() - onset, size_t(1.5 * audio.rate));
    t.mono.assign(audio.mono.begin() + onset, audio.mono.begin() + onset + len);
    t.profile = ProfileDrum(t.mono, audio.rate);
    targets.push_back(std::move(t));
  }
  if (targets.empty()) return 2;
  analysis::Audio first;
  analysis::ReadWav(targets[0].path.c_str(), first);
  const uint32_t rate = first.rate;

  synth::instruments::DrumKit kit;
  kit.set_sample_rate(rate);
  kit.Prepare();
  kit.LoadCalibration(cal_path);  // start from what is already fitted
  float f01 = 0;
  const bool pitched = use_pitch && kit.GetParam(note, "f01", &f01) && f01 > 0;
  std::vector<float> value(params.size());
  for (size_t i = 0; i < params.size(); ++i)
    if (!kit.GetParam(note, params[i].c_str(), &value[i])) { fprintf(stderr, "unknown parameter %s\n", params[i].c_str()); return 1; }

  auto evaluate = [&]() {
    double e = 0;
    for (const FitTarget& t : targets) {
      auto syn = RenderKitNote(kit, note, t.velocity, t.mono.size());
      e += ProfileError(t.profile, ProfileDrum(syn, rate), pitched);
    }
    return e / targets.size();
  };
  auto print = [&](const char* label, double e) {
    printf("%-8s error %6.2f :", label, e);
    for (size_t i = 0; i < params.size(); ++i) printf(" %s=%.4g", params[i].c_str(), value[i]);
    printf("\n");
  };
  double best = evaluate();
  print("start", best);
  double step = 1.0;  // log2 units: first tries halve / double, then finer
  for (int round = 0; round < rounds; ++round) {
    bool improved = false;
    for (size_t i = 0; i < params.size(); ++i) {
      for (int dir = -1; dir <= 1; dir += 2) {
        const float old = value[i];
        float trial = old * float(pow(2.0, dir * step));
        if (old == 0) trial = dir > 0 ? 0.01f : 0.0f;
        kit.SetParam(note, params[i].c_str(), trial);
        double e = evaluate();
        if (e < best - 1e-4) {
          best = e;
          value[i] = trial;
          improved = true;
          break;
        }
        kit.SetParam(note, params[i].c_str(), old);
      }
    }
    char label[16];
    sprintf_s(label, "round %d", round + 1);
    print(label, best);
    if (!improved) step *= 0.5;
  }

  // Merge into the calibration file.
  std::vector<std::string> lines;
  FILE* f = nullptr;
  if (fopen_s(&f, cal_path, "r") == 0 && f) {
    char line[512];
    while (fgets(line, sizeof(line), f)) {
      int n = 0;
      char key[64] = {};
      if (sscanf_s(line, "%d %63s", &n, key, unsigned(sizeof(key))) == 2 && n == note &&
          std::find(params.begin(), params.end(), std::string(key)) != params.end())
        continue;
      std::string s = line;
      while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
      lines.push_back(s);
    }
    fclose(f);
  }
  if (lines.empty()) {
    lines.push_back("# Drum kit calibration: \"note parameter value\" lines applied to the physical kit (drum_kit.cpp).");
    lines.push_back("# Rebuild with DSP_Render drumfit (see README.md).");
    lines.push_back("name VCSL drums");
  }
  for (size_t i = 0; i < params.size(); ++i) {
    char line[128];
    sprintf_s(line, "%d %s %.5g", note, params[i].c_str(), value[i]);
    lines.push_back(line);
  }
  if (fopen_s(&f, cal_path, "w") != 0 || !f) { fprintf(stderr, "cannot write %s\n", cal_path); return 1; }
  for (auto& l : lines) fprintf(f, "%s\n", l.c_str());
  fclose(f);
  printf("wrote %s\n", cal_path);
  return 0;
}

// A short drum performance on channel 10 (no file needed): rock groove with ghost notes and
// hi-hat accents, open/closed/pedal hi-hat chokes, a tom fill into a crash, a ride section with
// bell accents, a snare roll and a final crash + kick. Shows velocity, round-robin and chokes.
int CmdDrumGroove(int argc, char** argv) {
  if (argc < 3) return 2;
  const uint32_t rate = argc > 3 ? atoi(argv[3]) : 48000;
  const double bpm = argc > 4 ? atof(argv[4]) : 100;
  struct Hit { double beat; int note, vel; };
  std::vector<Hit> hits;
  auto add = [&](double beat, int note, int vel) { hits.push_back({beat, note, vel}); };
  double bar = 0;
  // Bars 1-4: groove, eighth-note hats with accents, ghost notes on the snare.
  for (int b = 0; b < 4; ++b, bar += 4) {
    for (int e = 0; e < 8; ++e) {
      const bool open = b == 3 && e == 7;
      add(bar + e * 0.5, open ? 46 : 42, e % 2 ? 62 : 96);
    }
    add(bar + 0, 36, 112); add(bar + 1.5, 36, 92); add(bar + 2, 36, 108);
    add(bar + 1, 38, 110); add(bar + 3, 38, 116);
    add(bar + 1.75, 38, 34); add(bar + 3.75, 38, 30);
    if (b == 1) add(bar + 2.75, 38, 40);
  }
  add(bar, 44, 80);  // pedal closes the open hat
  // Bar 5: tom fill (16ths) down the kit.
  const int toms[] = {50, 50, 48, 48, 47, 47, 45, 45, 43, 43, 41, 41, 38, 38, 41, 41};
  for (int i = 0; i < 16; ++i) add(bar + i * 0.25, toms[i], 84 + (i % 2 ? 0 : 20));
  add(bar + 0, 36, 100); add(bar + 2, 36, 100);
  bar += 4;
  // Bars 6-9: ride with bell accents, crash on the one, pedal hat on 2 and 4.
  add(bar, 49, 120); add(bar, 36, 120);
  for (int b = 0; b < 4; ++b, bar += 4) {
    for (int q = 0; q < 4; ++q) {
      add(bar + q, q == 0 ? 53 : 51, q == 0 ? 96 : 84);
      add(bar + q + 0.5, 51, 64);
    }
    add(bar + 1, 44, 70); add(bar + 3, 44, 70);
    add(bar + 0, 36, 108); add(bar + 2.5, 36, 96);
    add(bar + 1, 38, 104); add(bar + 3, 38, 110);
  }
  // Bar 10: crescendo snare roll (32nds) into crash + kick + china.
  for (int i = 0; i < 32; ++i) add(bar + i * 0.125, 38, 30 + i * 3);
  bar += 4;
  add(bar, 57, 124); add(bar, 36, 124); add(bar + 0.5, 52, 100); add(bar + 1, 55, 96);
  std::sort(hits.begin(), hits.end(), [](const Hit& a, const Hit& b) { return a.beat < b.beat; });

  Engine e(rate);
  synth::effects::MasterLimiter limiter;
  limiter.Configure(rate);
  std::vector<float> out;
  const double sec_per_beat = 60.0 / bpm;
  float min_gain = 1;
  LARGE_INTEGER t0, t1, freq;
  QueryPerformanceFrequency(&freq);
  QueryPerformanceCounter(&t0);
  size_t frame = 0;
  float block[kBlock * 2];
  auto render = [&](size_t until) {
    while (frame < until) {
      uint32_t n = uint32_t(std::min<size_t>(kBlock, until - frame));
      e.synth.RenderIdleStereo(n, block);
      min_gain = std::min(min_gain, limiter.Process(block, n));
      out.insert(out.end(), block, block + n * 2);
      frame += n;
    }
  };
  for (const Hit& h : hits) {
    render(size_t(h.beat * sec_per_beat * rate));
    e.synth.HandleMidiMessage(0x99, uint8_t(h.note), uint8_t(h.vel));
    e.synth.HandleMidiMessage(0x89, uint8_t(h.note), 0);
  }
  render(frame + size_t(4.0 * rate));
  QueryPerformanceCounter(&t1);
  const double cpu_s = double(t1.QuadPart - t0.QuadPart) / freq.QuadPart, seconds = frame / double(rate);
  float peak = 0;
  for (float v : out) peak = std::max(peak, fabsf(v));
  printf("%s: %zu hits, %.1f s audio in %.2f s (%.0fx realtime, %.2f%% of one core), peak %.1f dBFS, limiter max %.1f dB\n",
         argv[2], hits.size(), seconds, cpu_s, seconds / cpu_s, 100 * cpu_s / seconds, 20 * log10(peak + 1e-12),
         20 * log10(min_gain));
  return WriteWav(argv[2], out, rate) ? 0 : 1;
}

// CPU per drum piece: every hit re-strikes the piece at `rate_hz` so it never stops sounding.
int CmdDrumCpu(int argc, char** argv) {
  const double seconds = argc > 2 ? atof(argv[2]) : 5.0;
  const uint32_t rate = argc > 3 ? atoi(argv[3]) : 48000;
  printf("CPU per drum voice at %u Hz (one piece sounding, re-struck every 250 ms, %.0f s each)\n", rate, seconds);
  printf("note  name                    piece                 modes   %% of one core\n");
  static const int notes[] = {36, 38, 42, 46, 49, 51, 45, 52, 54, 69, 81};
  float block[kBlock * 2];
  LARGE_INTEGER freq;
  QueryPerformanceFrequency(&freq);
  for (int note : notes) {
    Engine e(rate);
    synth::instruments::DrumKit kit_info;
    kit_info.set_sample_rate(rate);
    kit_info.Prepare();
    const uint32_t blocks = uint32_t(seconds * rate / kBlock), hit_every = rate / 4 / kBlock;
    LARGE_INTEGER t0, t1;
    QueryPerformanceCounter(&t0);
    for (uint32_t b = 0; b < blocks; ++b) {
      if (b % hit_every == 0) {
        e.synth.HandleMidiMessage(0x99, uint8_t(note), 100);
        e.synth.HandleMidiMessage(0x89, uint8_t(note), 0);
      }
      e.synth.RenderIdleStereo(kBlock, block);
    }
    QueryPerformanceCounter(&t1);
    const double cpu_s = double(t1.QuadPart - t0.QuadPart) / freq.QuadPart;
    const char* piece = synth::instruments::DrumKit::PieceName(note);
    printf("%4d  %-22s  %-20s  %5d   %6.3f%%\n", note, synth::instruments::DrumKit::DrumName(note), piece ? piece : "-",
           kit_info.PieceModes(note), 100 * cpu_s / (double(blocks) * kBlock / rate));
  }
  // Baseline: the same engine with nothing playing.
  Engine e(rate);
  const uint32_t blocks = uint32_t(seconds * rate / kBlock);
  LARGE_INTEGER t0, t1;
  QueryPerformanceCounter(&t0);
  for (uint32_t b = 0; b < blocks; ++b) e.synth.RenderIdleStereo(kBlock, block);
  QueryPerformanceCounter(&t1);
  printf("(idle engine, no notes: %.3f%%)\n", 100 * double(t1.QuadPart - t0.QuadPart) / freq.QuadPart / (double(blocks) * kBlock / rate));
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
    else if (!strcmp(argv[1], "drums")) result = CmdDrums(argc, argv);
    else if (!strcmp(argv[1], "drum")) result = CmdDrumInfo(argc, argv);
    else if (!strcmp(argv[1], "drumcompare")) result = CmdDrumCompare(argc, argv);
    else if (!strcmp(argv[1], "drumcpu")) result = CmdDrumCpu(argc, argv);
    else if (!strcmp(argv[1], "drumgroove")) result = CmdDrumGroove(argc, argv);
    else if (!strcmp(argv[1], "drumfit")) result = CmdDrumFit(argc, argv);
    else if (!strcmp(argv[1], "info")) result = CmdInfo(argc, argv);
    else if (!strcmp(argv[1], "analyze")) result = CmdAnalyze(argc, argv);
    else if (!strcmp(argv[1], "survey")) result = CmdSurvey(argc, argv);
    else if (!strcmp(argv[1], "calibrate")) result = CmdCalibrate(argc, argv);
    else if (!strcmp(argv[1], "compare")) result = CmdCompare(argc, argv);
    else if (!strcmp(argv[1], "design")) result = CmdDesign(argc, argv);
  }
  if (result == 2) {
    fprintf(stderr,
            "usage:\n"
            "  DSP_Render render <in.mid> <out.wav> [rate] [program 1-128 for every part]\n"
            "  DSP_Render note <program 1-128|0=drums> <note> <velocity> <hold_s> <out.wav> [tail_s] [rate]\n"
            "  DSP_Render tune <program> [first_note] [last_note] [step] [rate]\n"
            "  DSP_Render cpu <program> <voices> [seconds] [rate]\n"
            "  DSP_Render drums [rate] [wav_dir|-] [velocity] [first_note] [last_note]\n"
            "  DSP_Render drum <note> [velocity] [rate]\n"
            "  DSP_Render drumcompare <note> <recording.wav> [ab_out.wav|-] [velocity]\n"
            "  DSP_Render drumcpu [seconds] [rate]\n"
            "  DSP_Render drumgroove <out.wav> [rate] [bpm]\n"
            "  DSP_Render info   (calibration folder and calibrated instruments in use)\n"
            "  DSP_Render drumfit <cal file> <note> <param,param,...> <recording.wav>@<velocity> ... [-rounds N]\n"
            "  DSP_Render analyze <recording.wav> [midi_note]\n"
            "  DSP_Render survey <folder of recordings>\n"
            "  DSP_Render calibrate <folder of recordings> <out.cal> [excitation_ms]\n"
            "  DSP_Render compare <program> <recording.wav> [ab_out.wav|-] [velocity]\n");
  }
  return result;
}
