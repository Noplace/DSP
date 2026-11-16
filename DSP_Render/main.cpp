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
//   DSP_Render release <key-up.wav> <sustained.wav> [key_up_out.wav|-] [env]
//       Split a key-up recording into string and key-up sound; damper timing and decay.
//   DSP_Render keyup <program> <key-up.wav> <sustained.wav> <ab_out.wav> [velocity]
//       The synth's key-up against a recording's (same hold, same scale), with an A/B file.
//   DSP_Render clicks <render.wav> [ratio]
//       List clicks (steps or kinks in the waveform) in a render.
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

// Folder of key-up recordings for a folder of sustained ones: the given one ("-" = none), else a
// sibling folder named "release" if there is one.
std::string ReleaseFolder(const char* sustain_folder, const char* given) {
  if (given) return strcmp(given, "-") ? std::string(given) : std::string();
  std::string dir = sustain_folder;
  while (!dir.empty() && (dir.back() == '\\' || dir.back() == '/')) dir.pop_back();
  const size_t slash = dir.find_last_of("\\/");
  std::string sibling = (slash == std::string::npos ? std::string(".") : dir.substr(0, slash)) + "\\release";
  const DWORD attr = GetFileAttributesA(sibling.c_str());
  return attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY) ? sibling : std::string();
}

// Builds a calibration file from a folder of recorded notes: measured string parameters per note
// plus the excitation (pluck + body) obtained by inverse-filtering each recording with the fitted
// string loop. Key-up recordings (a "release" folder next to it, or the one given) add each note's
// damper timing and decay and the key-up sound per velocity layer.
int CmdCalibrate(int argc, char** argv) {
  if (argc < 4) return 2;
  const double exc_ms = argc > 4 && strcmp(argv[4], "-") ? atof(argv[4]) : 200.0;
  auto files = CollectRecordings(argv[2]);
  if (files.empty()) { fprintf(stderr, "no WAV files with note names in %s\n", argv[2]); return 1; }
  int shift = ConsensusOctave(files);
  const std::string release_dir = ReleaseFolder(argv[2], argc > 5 ? argv[5] : nullptr);
  std::vector<std::pair<int, std::string>> release_files;
  if (!release_dir.empty()) {
    release_files = CollectRecordings(release_dir.c_str());
    printf("key-up recordings: %zu in %s\n", release_files.size(), release_dir.c_str());
  }
  // Key-up recording of a named note and velocity layer, or empty.
  auto release_file = [&](int named, int layer) {
    for (auto& [n, path] : release_files)
      if (n == named && std::max(1, analysis::VelocityLayerFromFileName(path.c_str())) == layer) return path;
    return std::string();
  };
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
    std::vector<std::pair<double, double>> damper_t60, damper_delay;  // (value, dB range it was measured over)
    std::string release_log;
    for (auto& [layer, path] : layers) {
      analysis::Audio la;
      analysis::NoteAnalysis lna;
      if (!analysis::ReadWav(path.c_str(), la) || !analysis::AnalyzeNote(la, named, lna, 40, shift)) continue;
      // Key-up sound of this layer (the string's pitch from the note's analysis).
      std::vector<float> key_up;
      const std::string rel_path = release_file(named, layer);
      analysis::Audio ra;
      analysis::ReleaseAnalysis rel;
      if (!rel_path.empty() && analysis::ReadWav(rel_path.c_str(), ra) && ra.rate == la.rate &&
          analysis::AnalyzeRelease(ra, a.f0_hz, a.inharmonicity, rel)) {
        key_up = std::move(rel.key_up);
        if (rel.damper_t60_s > 0) {
          damper_t60.push_back({rel.damper_t60_s, rel.string_snr_db});
          damper_delay.push_back({rel.damper_delay_s, rel.string_snr_db});
        }
        char buf[128];
        sprintf_s(buf, "  vl%d T60 %.2f s +%2.0f ms (%2.0f dB), string %5.1f dB, key-up %5.1f dB %3.0f ms", layer,
                  rel.damper_t60_s, rel.damper_delay_s * 1000, rel.string_snr_db, rel.string_db, rel.key_up_db,
                  key_up.size() * 1000.0 / ra.rate);
        release_log += buf;
      } else if (!rel_path.empty()) {
        release_log += "  (" + rel_path + ": analysis failed)";
      }
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
      el.release = std::move(key_up);
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
    // Dampers: the same for every layer (the layers only differ in how loud the string still was),
    // so the median over the layers, weighted by how far down each could follow the decay.
    auto median = [](std::vector<std::pair<double, double>> v) {
      std::sort(v.begin(), v.end());
      double total = 0, acc = 0;
      for (auto& p : v) total += p.second;
      for (auto& p : v)
        if ((acc += p.second) >= 0.5 * total) return p.first;
      return 0.0;
    };
    cn.release_t60_s = float(median(damper_t60));
    cn.damper_delay_s = float(median(damper_delay));
    if (!release_log.empty())
      printf("      key-up:%s  -> dampers T60 %.2f s after %.0f ms\n", release_log.c_str(), cn.release_t60_s,
             cn.damper_delay_s * 1000);
    // Single-layer instruments keep the plain excitation field (version 1 behaviour).
    if (cn.layers.size() == 1) {
      cn.excitation = std::move(cn.layers[0].samples);
      cn.partials = std::move(cn.layers[0].partials);
      cn.release = std::move(cn.layers[0].release);
      cn.layers.clear();
    }
    cal.notes.push_back(std::move(cn));
  }
  if (!cal.Save(argv[3])) { fprintf(stderr, "cannot write %s\n", argv[3]); return 1; }
  size_t samples = 0, release_samples = 0;
  for (auto& n : cal.notes) {
    samples += n.excitation.size();
    release_samples += n.release.size();
    for (auto& l : n.layers) { samples += l.samples.size(); release_samples += l.release.size(); }
  }
  printf("wrote %s: %zu notes, %.0f KB of excitation, %.0f KB of key-up sounds\n", argv[3], cal.notes.size(),
         samples * 4 / 1024.0, release_samples * 4 / 1024.0);
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
  for (const auto& c : e.synth.calibrated_instruments()) printf("calibrated: %s\n", c.c_str());
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
  // Optional "solo:<group>" / "mute:<group>" (drum mixer groups, e.g. solo:kick): checks the
  // kit's group strips.
  for (int i = 5; i < argc; ++i) {
    const bool solo = !strncmp(argv[i], "solo:", 5), mute = !strncmp(argv[i], "mute:", 5);
    if (!solo && !mute) continue;
    for (int g = 0; g < synth::kDrumGroupCount; ++g)
      if (!_stricmp(argv[i] + 5, synth::DrumGroupName(g)))
        e.synth.mixer.strip(synth::effects::Mixer::kKitFirst + g).set(solo ? synth::effects::kStripSolo : synth::effects::kStripMute, 1);
  }
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
    if (!e.synth.calibration_dir().empty()) kit_info.LoadCalibration((e.synth.calibration_dir() + "\\drums_vcsl.cal").c_str());
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
    printf("%4d  %-22s  %-20s  %5d   %6.3f%%  %s\n", note, synth::instruments::DrumKit::DrumName(note), piece ? piece : "-",
           kit_info.PieceModes(note), 100 * cpu_s / (double(blocks) * kBlock / rate),
           kit_info.NoteIsRecorded(note) ? "recorded (mode slots + residual)" : "modelled");
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

// Splits a key-up recording into the string (removed) and the key-up sound, using the pitch of the
// matching sustained recording; optionally writes the key-up sound to a WAV.
int CmdRelease(int argc, char** argv) {
  if (argc < 4) return 2;
  analysis::Audio rel, sus;
  analysis::NoteAnalysis a;
  if (!analysis::ReadWav(argv[2], rel) || !analysis::ReadWav(argv[3], sus)) { fprintf(stderr, "cannot read the recordings\n"); return 1; }
  if (!analysis::AnalyzeNote(sus, analysis::NoteFromFileName(argv[3]), a)) { fprintf(stderr, "cannot analyse %s\n", argv[3]); return 1; }
  analysis::ReleaseAnalysis r;
  if (!analysis::AnalyzeRelease(rel, a.f0_hz, a.inharmonicity, r)) { fprintf(stderr, "cannot analyse %s\n", argv[2]); return 1; }
  char name[8];
  printf("%s (MIDI %d, f0 %.2f Hz): string at key-up %.1f dB, %d partials removed (%.1f dB of the file)\n",
         NoteName(a.midi_note, name, sizeof(name)), a.midi_note, a.f0_hz, r.string_db, r.partials, r.removed_db);
  printf("dampers: touch %.0f ms after key-up, then T60 %.3f s (%d partials, followed %.0f dB down); key-up sound peak %.1f dB (40 ms RMS), %.0f ms long\n",
         r.damper_delay_s * 1000, r.damper_t60_s, r.timed_partials, r.string_snr_db, r.key_up_db, r.key_up.size() * 1000.0 / rel.rate);
  if (argc > 4 && strcmp(argv[4], "-")) {
    std::vector<float> stereo;
    for (float v : r.key_up) { stereo.push_back(v); stereo.push_back(v); }
    WriteWav(argv[4], stereo, rel.rate);
  }
  if (argc > 5) {  // the string's level after key-up, every 10 ms
    printf("string level (dB, every 10 ms):");
    const size_t every = std::max<size_t>(1, size_t(0.01 / r.env_dt_s + 0.5));
    for (size_t i = 0; i < r.string_db_env.size(); i += every) printf("%s%.0f", i % (every * 20) ? " " : "\n  ", r.string_db_env[i]);
    printf("\n");
  }
  return 0;
}

// 20 ms RMS (dB) every 10 ms.
std::vector<double> RmsEnvelopeDb(const std::vector<float>& x, uint32_t rate) {
  std::vector<double> env;
  const size_t step = rate / 100, win = rate / 50;
  for (size_t i = 0; i + win <= x.size(); i += step) {
    double e = 0;
    for (size_t j = i; j < i + win; ++j) e += double(x[j]) * x[j];
    env.push_back(10 * log10(e / win + 1e-30));
  }
  return env;
}

// Key-up of the synth against a key-up recording. The recording's note was held until its
// string had decayed to the level the recording starts at; the sustained recording of the same
// note and layer says how long that took, so the synth holds its note as long, then lets go.
// Both are split the same way (string removed, key-up sound left) and printed side by side; the
// A/B file has the recording, a gap, then the synth, on a common scale (the synth matched to the
// sustained recording's attack peak), so string and key-up levels compare directly.
int CmdKeyUp(int argc, char** argv) {
  if (argc < 6) return 2;
  const int program = atoi(argv[2]);
  const char* rel_path = argv[3];
  const char* sus_path = argv[4];
  const char* ab_path = argv[5];
  analysis::Audio rel, sus;
  analysis::NoteAnalysis a;
  if (!analysis::ReadWav(rel_path, rel) || !analysis::ReadWav(sus_path, sus) || rel.rate != sus.rate) {
    fprintf(stderr, "cannot read the recordings (or their rates differ)\n");
    return 1;
  }
  if (!analysis::AnalyzeNote(sus, analysis::NoteFromFileName(sus_path), a)) { fprintf(stderr, "cannot analyse %s\n", sus_path); return 1; }
  const int layer = analysis::VelocityLayerFromFileName(rel_path);
  const int velocity = argc > 6 ? atoi(argv[6]) : layer > 0 ? std::min(127, int(layer / 4.0 * 127 + 0.5)) : 100;
  analysis::ReleaseAnalysis rr;
  if (!analysis::AnalyzeRelease(rel, a.f0_hz, a.inharmonicity, rr)) { fprintf(stderr, "cannot analyse %s\n", rel_path); return 1; }

  // Hold time: when the sustained recording has decayed to the key-up recording's first 20 ms.
  const std::vector<double> sus_env = RmsEnvelopeDb(sus.mono, sus.rate);
  size_t peak_i = 0;
  for (size_t i = 0; i < sus_env.size() && i < 50; ++i) if (sus_env[i] > sus_env[peak_i]) peak_i = i;
  size_t hold_i = peak_i;
  while (hold_i + 1 < sus_env.size() && sus_env[hold_i] > rr.string_db) ++hold_i;
  const double onset_s = a.onset / double(sus.rate);
  const double hold_s = std::max(0.3, hold_i * 0.01 - onset_s);
  float sus_peak = 0;
  for (float v : sus.mono) sus_peak = std::max(sus_peak, fabsf(v));

  Engine e(rel.rate);
  const double tail_s = rel.mono.size() / double(rel.rate);
  std::vector<float> syn = ToMono(RenderNote(e, program, a.midi_note, velocity, hold_s, tail_s));
  float syn_peak = 1e-9f;
  for (float v : syn) syn_peak = std::max(syn_peak, fabsf(v));
  const float k = sus_peak / syn_peak;  // synth -> recording scale
  const size_t off = std::min(syn.size(), size_t(hold_s * rel.rate + 0.5));
  analysis::Audio sa;
  sa.rate = rel.rate;
  sa.channels = 1;
  for (size_t i = off; i < syn.size() && sa.mono.size() < rel.mono.size(); ++i) sa.mono.push_back(syn[i] * k);
  analysis::ReleaseAnalysis sr;
  if (!analysis::AnalyzeRelease(sa, a.f0_hz, a.inharmonicity, sr)) { fprintf(stderr, "cannot analyse the synth's key-up\n"); return 1; }

  char name[8];
  const double ref = 20 * log10(sus_peak);
  printf("%s, MIDI %d (%s), velocity %d, program %d: held %.1f s (sustained recording down to the key-up recording's level)\n",
         rel_path, a.midi_note, NoteName(a.midi_note, name, sizeof(name)), velocity, program, hold_s);
  printf("levels in dB vs the attack peak     recording   synth\n");
  printf("  string at key-up (first 20 ms)    %8.1f  %7.1f\n", rr.string_db - ref, sr.string_db - ref);
  printf("  key-up sound (loudest 40 ms)      %8.1f  %7.1f\n", rr.key_up_db - ref, sr.key_up_db - ref);
  printf("  dampers touch after (ms)          %8.0f  %7.0f\n", rr.damper_delay_s * 1000, sr.damper_delay_s * 1000);
  printf("  damper T60 (s)                    %8.3f  %7.3f\n", rr.damper_t60_s, sr.damper_t60_s);
  printf("string level after key-up (dB vs attack peak), every 50 ms:\n  rec:");
  const size_t every = std::max<size_t>(1, size_t(0.05 / rr.env_dt_s + 0.5));
  for (size_t i = 0; i < rr.string_db_env.size() && i * rr.env_dt_s < 0.8; i += every) printf(" %4.0f", rr.string_db_env[i] - ref);
  printf("\n  syn:");
  for (size_t i = 0; i < sr.string_db_env.size() && i * sr.env_dt_s < 0.8; i += every) printf(" %4.0f", sr.string_db_env[i] - ref);
  printf("\n");

  // A/B: both scaled so the attack peak would sit at -6 dBFS; the key-up recordings are quiet.
  const float g = 0.5f / sus_peak;
  std::vector<float> ab;
  for (float v : rel.mono) { ab.push_back(v * g); ab.push_back(v * g); }
  ab.insert(ab.end(), size_t(rel.rate) * 2 / 2, 0.0f);
  for (float v : sa.mono) { ab.push_back(v * g); ab.push_back(v * g); }
  if (WriteWav(ab_path, ab, rel.rate)) printf("A/B written to %s (recording, 0.5 s gap, synth)\n", ab_path);
  return 0;
}

// Finds clicks in a render: 5 ms blocks whose peak second difference (a step or a kink in the
// waveform) stands far above the blocks around them. Prints each one and the count.
int CmdClicks(int argc, char** argv) {
  if (argc < 3) return 2;
  // Hammer attacks reach about 3x the blocks around them; a click (a step of 1% of full scale in
  // a mezzo-forte passage) far more.
  const double ratio = argc > 3 ? atof(argv[3]) : 8.0;
  analysis::Audio a;
  if (!analysis::ReadWav(argv[2], a)) { fprintf(stderr, "cannot read %s\n", argv[2]); return 1; }
  const size_t block = std::max<uint32_t>(1, a.rate / 200);
  std::vector<double> peak, rms;
  for (size_t b = 2; b + block <= a.mono.size(); b += block) {
    double p = 0, e = 0;
    for (size_t i = b; i < b + block; ++i) {
      const double d = a.mono[i] - 2.0 * a.mono[i - 1] + a.mono[i - 2];
      p = std::max(p, fabs(d));
      e += d * d;
    }
    peak.push_back(p);
    rms.push_back(sqrt(e / block));
  }
  int clicks = 0;
  for (size_t b = 4; b + 4 < peak.size(); ++b) {
    std::vector<double> around;
    for (size_t k = b - 4; k <= b + 4; ++k) if (k != b) around.push_back(rms[k]);
    std::sort(around.begin(), around.end());
    const double local = around[around.size() / 2];
    if (peak[b] > 1e-4 && peak[b] > ratio * local && peak[b] > 3.0 * rms[b]) {
      if (++clicks <= 40) printf("  %.3f s: second difference %.2e vs %.2e around\n", (b * block + 2) / double(a.rate), peak[b], local);
    }
  }
  printf("%s: %d click(s) (%.1f s)\n", argv[2], clicks, a.mono.size() / double(a.rate));
  return 0;
}

}  // namespace

int CmdDrumCalibrate(int argc, char** argv);  // drum_calibrate.cpp
int CmdDrumCalInfo(int argc, char** argv);    // drum_calibrate.cpp

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
    else if (!strcmp(argv[1], "drumcal")) result = CmdDrumCalibrate(argc, argv);
    else if (!strcmp(argv[1], "drumcalinfo")) result = CmdDrumCalInfo(argc, argv);
    else if (!strcmp(argv[1], "info")) result = CmdInfo(argc, argv);
    else if (!strcmp(argv[1], "analyze")) result = CmdAnalyze(argc, argv);
    else if (!strcmp(argv[1], "survey")) result = CmdSurvey(argc, argv);
    else if (!strcmp(argv[1], "calibrate")) result = CmdCalibrate(argc, argv);
    else if (!strcmp(argv[1], "compare")) result = CmdCompare(argc, argv);
    else if (!strcmp(argv[1], "design")) result = CmdDesign(argc, argv);
    else if (!strcmp(argv[1], "release")) result = CmdRelease(argc, argv);
    else if (!strcmp(argv[1], "keyup")) result = CmdKeyUp(argc, argv);
    else if (!strcmp(argv[1], "clicks")) result = CmdClicks(argc, argv);
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
            "  DSP_Render drumgroove <out.wav> [rate] [bpm] [solo:<group>|mute:<group> ...]  (kick snare toms hi-hat cymbals ride percussion fx)\n"
            "  DSP_Render info   (calibration folder and calibrated instruments in use)\n"
            "  DSP_Render drumcal <manifest.txt> <out.cal>   (drum calibration from recorded hits)\n"
            "  DSP_Render analyze <recording.wav> [midi_note]\n"
            "  DSP_Render survey <folder of recordings>\n"
            "  DSP_Render calibrate <folder of recordings> <out.cal> [excitation_ms|-] [key-up folder|-]\n"
            "      (key-up recordings: a sibling folder named 'release' unless given; '-' = none)\n"
            "  DSP_Render release <key-up recording.wav> <sustained recording.wav> [key_up_out.wav]\n"
            "  DSP_Render compare <program> <recording.wav> [ab_out.wav|-] [velocity]\n");
  }
  return result;
}
