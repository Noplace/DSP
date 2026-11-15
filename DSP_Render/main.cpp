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
  printf("note  layers    f0 Hz        B   T60 f0  T60 2k  disp rms/max, filter | per layer: velocity, string removed, exc peak\n");
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
    if (a.after_db > -40 && a.after_db < -3) {  // a clear aftersound: two strings
      // First string keeps the overall decay -- the loop the excitation is recovered with, so
      // resynthesis stays consistent; the second string adds the slower aftersound.
      cn.after_b1 = float(std::max(0.005, a.after_b1));
      cn.after_b3 = float(std::max(0.0, a.after_b3));
      double ratio = pow(10.0, a.after_db / 20.0);
      cn.after_level = float(ratio / (1.0 + ratio));
      cn.detune_cents = 0.0f;  // set by the instrument (beating is not measured yet)
    }

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
      el.samples.assign(residual.begin(), residual.begin() + std::min(exc_len, residual.size()));
      const size_t fade = el.samples.size() / 4;  // half-Hann fade over the last quarter
      for (size_t i = 0; i < fade; ++i) {
        double w = 0.5 + 0.5 * cos(3.14159265358979 * (i + 1) / fade);
        el.samples[el.samples.size() - fade + i] *= float(w);
      }
      float exc_peak = 0;
      for (float v : el.samples) exc_peak = std::max(exc_peak, fabsf(v));
      printf("  %.2f %4.1f dB %5.1f dB", el.velocity, 10 * log10(ey / ee), 20 * log10(exc_peak / std::max(1e-9f, el.peak)));
      cn.layers.push_back(std::move(el));
    }
    printf("\n");
    // Single-layer instruments keep the plain excitation field (version 1 behaviour).
    if (cn.layers.size() == 1) {
      cn.excitation = std::move(cn.layers[0].samples);
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
  const char* wav_dir = argc > 3 ? argv[3] : nullptr;  // optional: write one WAV per drum
  printf("GM drum kit at %u Hz, velocity 100\n", rate);
  printf("note  name                    peak dBFS   -20dB s  -40dB s\n");
  for (int note = 27; note <= 87; ++note) {
    const char* name = synth::instruments::ModalInstrument::DrumName(note);
    if (!name) continue;
    Engine e(rate);
    auto stereo = RenderNote(e, 0, note, 100, 0.1, 2.9);
    auto mono = ToMono(stereo);
    float peak = 0;
    for (float v : mono) peak = std::max(peak, fabsf(v));
    Decay d = MeasureDecay(mono, rate, 3.0);
    printf("%4d  %-22s  %8.1f  %8.2f  %7.2f\n", note, name, 20 * log10(peak + 1e-12), d.t20, d.t40);
    if (wav_dir) {
      char path[MAX_PATH];
      sprintf_s(path, "%s\\drum_%02d.wav", wav_dir, note);
      WriteWav(path, stereo, rate);
    }
  }
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
            "  DSP_Render drums [rate] [wav_dir]\n"
            "  DSP_Render analyze <recording.wav> [midi_note]\n"
            "  DSP_Render survey <folder of recordings>\n"
            "  DSP_Render calibrate <folder of recordings> <out.cal> [excitation_ms]\n"
            "  DSP_Render compare <program> <recording.wav> [ab_out.wav|-] [velocity]\n");
  }
  return result;
}
