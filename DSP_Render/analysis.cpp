// Analysis of recorded notes, for calibrating physical models against real instruments.
#define NOMINMAX
#include "analysis.h"

#include <algorithm>
#include <complex>
#include <math.h>
#include <stdio.h>
#include <string.h>

namespace analysis {

namespace {

const double kPi = 3.14159265358979323846;
typedef std::complex<double> cd;

void Fft(std::vector<cd>& a) {
  const size_t n = a.size();
  for (size_t i = 1, j = 0; i < n; ++i) {
    size_t bit = n >> 1;
    for (; j & bit; bit >>= 1) j ^= bit;
    j ^= bit;
    if (i < j) std::swap(a[i], a[j]);
  }
  for (size_t len = 2; len <= n; len <<= 1) {
    cd wl = std::polar(1.0, -2.0 * kPi / len);
    for (size_t i = 0; i < n; i += len) {
      cd w(1);
      for (size_t k = 0; k < len / 2; ++k) {
        cd u = a[i + k], v = a[i + k + len / 2] * w;
        a[i + k] = u + v;
        a[i + k + len / 2] = u - v;
        w *= wl;
      }
    }
  }
}

double ToDb(double x) { return 20.0 * log10(std::max(x, 1e-12)); }

// Magnitude spectrum (dB) of a Hann-windowed segment, zero-padded for fine peak interpolation.
struct Spectrum {
  std::vector<double> db;
  double bin_hz = 0;
  double FindPeak(double lo_hz, double hi_hz, double* level_db) const {
    int lo = std::max(1, int(lo_hz / bin_hz)), hi = std::min(int(db.size()) - 2, int(hi_hz / bin_hz) + 1);
    if (hi <= lo) { *level_db = -300; return 0; }
    int best = lo;
    for (int i = lo; i <= hi; ++i) if (db[i] > db[best]) best = i;
    double a = db[best - 1], b = db[best], c = db[best + 1];
    double denom = a - 2 * b + c, offset = denom < 0 ? 0.5 * (a - c) / denom : 0;
    *level_db = b - 0.25 * (a - c) * offset;
    return (best + offset) * bin_hz;
  }
};

Spectrum ComputeSpectrum(const std::vector<float>& x, size_t start, size_t length, uint32_t rate, size_t fft_size) {
  std::vector<cd> buf(fft_size, cd(0));
  double wsum = 0;
  for (size_t i = 0; i < length && start + i < x.size(); ++i) {
    double w = 0.5 - 0.5 * cos(2 * kPi * i / (length - 1));
    buf[i] = x[start + i] * w;
    wsum += w;
  }
  Fft(buf);
  Spectrum s;
  s.bin_hz = double(rate) / fft_size;
  s.db.resize(fft_size / 2);
  for (size_t i = 0; i < s.db.size(); ++i) s.db[i] = ToDb(2.0 * std::abs(buf[i]) / wsum);
  return s;
}

// Envelope (linear amplitude) of the signal component near `freq_hz`: heterodyne to DC, then a
// Hann-weighted average over `frame` samples every `hop` samples.
std::vector<double> BandEnvelope(const std::vector<float>& x, size_t start, uint32_t rate, double freq_hz,
                                 size_t frame, size_t hop) {
  std::vector<double> env;
  std::vector<double> w(frame);
  double wsum = 0;
  for (size_t i = 0; i < frame; ++i) { w[i] = 0.5 - 0.5 * cos(2 * kPi * i / (frame - 1)); wsum += w[i]; }
  const double dphi = 2 * kPi * freq_hz / rate;
  for (size_t f = start; f + frame <= x.size(); f += hop) {
    cd acc(0);
    cd rot = std::polar(1.0, -dphi), ph = std::polar(1.0, -dphi * double(f));
    for (size_t i = 0; i < frame; ++i) {
      acc += w[i] * double(x[f + i]) * ph;
      ph *= rot;
    }
    env.push_back(2.0 * std::abs(acc) / wsum);
  }
  return env;
}

// Least-squares line y = a + b x with weights. Returns false if degenerate.
bool FitLine(const std::vector<double>& x, const std::vector<double>& y, const std::vector<double>& w, double& a,
             double& b) {
  double sw = 0, sx = 0, sy = 0, sxx = 0, sxy = 0;
  for (size_t i = 0; i < x.size(); ++i) {
    sw += w[i]; sx += w[i] * x[i]; sy += w[i] * y[i]; sxx += w[i] * x[i] * x[i]; sxy += w[i] * x[i] * y[i];
  }
  double d = sw * sxx - sx * sx;
  if (sw <= 0 || fabs(d) < 1e-300) return false;
  b = (sw * sxy - sx * sy) / d;
  a = (sy - b * sx) / sw;
  return true;
}

}  // namespace

bool ReadWav(const char* path, Audio& audio, std::string* error) {
  auto fail = [&](const char* msg) { if (error) *error = msg; return false; };
  FILE* f = nullptr;
  if (fopen_s(&f, path, "rb") != 0 || !f) return fail("cannot open file");
  std::vector<uint8_t> data;
  fseek(f, 0, SEEK_END);
  data.resize(size_t(ftell(f)));
  fseek(f, 0, SEEK_SET);
  size_t got = fread(data.data(), 1, data.size(), f);
  fclose(f);
  if (got != data.size() || data.size() < 12 || memcmp(data.data(), "RIFF", 4) || memcmp(data.data() + 8, "WAVE", 4))
    return fail("not a RIFF/WAVE file");
  int format = 0, bits = 0;
  const uint8_t* pcm = nullptr;
  size_t pcm_bytes = 0;
  for (size_t pos = 12; pos + 8 <= data.size();) {
    uint32_t size;
    memcpy(&size, data.data() + pos + 4, 4);
    const uint8_t* body = data.data() + pos + 8;
    if (!memcmp(data.data() + pos, "fmt ", 4) && size >= 16) {
      uint16_t tag, ch, bps; uint32_t rate;
      memcpy(&tag, body, 2); memcpy(&ch, body + 2, 2); memcpy(&rate, body + 4, 4); memcpy(&bps, body + 14, 2);
      if (tag == 0xFFFE && size >= 26) memcpy(&tag, body + 24, 2);  // WAVE_FORMAT_EXTENSIBLE sub-format
      format = tag; audio.channels = ch; audio.rate = rate; bits = bps;
    } else if (!memcmp(data.data() + pos, "data", 4)) {
      pcm = body;
      pcm_bytes = std::min<size_t>(size, data.size() - (pos + 8));
    }
    pos += 8 + size + (size & 1);
  }
  if (!pcm || audio.channels <= 0 || audio.rate == 0) return fail("missing fmt or data chunk");
  const int bytes = bits / 8;
  if (!((format == 1 && (bits == 16 || bits == 24 || bits == 32)) || (format == 3 && bits == 32)))
    return fail("unsupported sample format (need PCM 16/24/32 or float32)");
  const size_t frames = pcm_bytes / (bytes * audio.channels);
  audio.mono.assign(frames, 0.0f);
  for (size_t i = 0; i < frames; ++i) {
    double sum = 0;
    for (int c = 0; c < audio.channels; ++c) {
      const uint8_t* p = pcm + (i * audio.channels + c) * bytes;
      double v;
      if (format == 3) { float fv; memcpy(&fv, p, 4); v = fv; }
      else if (bits == 16) { int16_t s; memcpy(&s, p, 2); v = s / 32768.0; }
      else if (bits == 24) { int32_t s = (p[0] << 8) | (p[1] << 16) | (p[2] << 24); v = (s >> 8) / 8388608.0; }
      else { int32_t s; memcpy(&s, p, 4); v = s / 2147483648.0; }
      sum += v;
    }
    audio.mono[i] = float(sum / audio.channels);
  }
  return true;
}

int NoteFromFileName(const char* path) {
  const char* name = path;
  for (const char* p = path; *p; ++p) if (*p == '\\' || *p == '/') name = p + 1;
  static const int semis[] = {9, 11, 0, 2, 4, 5, 7};  // A B C D E F G
  for (const char* p = name; *p; ++p) {
    if (*p < 'A' || *p > 'G') continue;
    bool boundary = (p == name) || p[-1] == '_' || p[-1] == ' ' || p[-1] == '-';
    if (!boundary) continue;
    const char* q = p + 1;
    int semi = semis[*p - 'A'];
    if (*q == '#') { ++semi; ++q; } else if (*q == 'b') { --semi; ++q; }
    bool negative = false;
    if (*q == '-') { negative = true; ++q; }
    if (*q < '0' || *q > '9') continue;
    int octave = *q - '0';
    if (negative) octave = -octave;
    ++q;
    if (*q != '_' && *q != '.' && *q != ' ' && *q != '-' && *q != 0) continue;
    return (octave + 1) * 12 + semi;
  }
  return -1;
}

bool AnalyzeNote(const Audio& audio, int midi_note, NoteAnalysis& r, int max_partials, int forced_shift) {
  const std::vector<float>& x = audio.mono;
  const uint32_t sr = audio.rate;
  if (x.size() < sr / 4 || midi_note < 0) return false;
  r = NoteAnalysis();
  r.midi_note = midi_note;
  r.expected_hz = 440.0 * pow(2.0, (midi_note - 69) / 12.0);

  // Onset: a few ms before the signal first reaches 5% of its peak.
  float peak = 0;
  for (float v : x) peak = std::max(peak, fabsf(v));
  size_t onset = 0;
  while (onset < x.size() && fabsf(x[onset]) < 0.05f * peak) ++onset;
  onset = onset > sr / 500 ? onset - sr / 500 : 0;
  r.onset = onset;

  // Long spectrum shortly after the attack (skip the first 30 ms of pluck transient).
  const size_t spec_start = onset + sr * 3 / 100;
  const size_t spec_len = std::min<size_t>(32768, x.size() - spec_start);
  Spectrum spec = ComputeSpectrum(x, spec_start, spec_len, sr, 131072);
  {
    std::vector<double> sorted(spec.db.begin() + size_t(50 / spec.bin_hz), spec.db.end());
    std::nth_element(sorted.begin(), sorted.begin() + sorted.size() / 2, sorted.end());
    r.noise_floor_db = sorted[sorted.size() / 2];
  }

  // Octave check: sample libraries disagree on octave numbering (some call middle C "C3"). Try the
  // named pitch an octave down, as named, and an octave up, and keep the lowest candidate whose
  // harmonic series is actually present (an octave too low only matches every other harmonic).
  double level;
  double f1 = 0;
  // "Present" means within 45 dB of the strongest component: small peaks from body resonances or
  // sympathetic strings sit far above the noise floor but well below the real partials.
  double strongest;
  spec.FindPeak(r.expected_hz * 0.4, 0.45 * sr, &strongest);
  const bool auto_octave = forced_shift == kAutoOctave;
  for (int shift : {-12, 0, 12}) {
    if (!auto_octave && shift != forced_shift) continue;
    double expected = 440.0 * pow(2.0, (midi_note + shift - 69) / 12.0);
    double fc = spec.FindPeak(expected * pow(2.0, -1.5 / 12), expected * pow(2.0, 1.5 / 12), &level);
    if (fc <= 0) continue;
    int hits = 0, tried = 0;
    for (int n = 1; n <= 10 && n * fc < 0.45 * sr; ++n, ++tried) {
      double lvl, f = spec.FindPeak(n * fc * pow(2.0, -0.4 / 12), n * fc * pow(2.0, 0.4 / 12), &lvl);
      if (f > 0 && lvl > strongest - 45 && fabs(1200 * log2(f / (n * fc))) < 30) ++hits;
    }
    if (!auto_octave || (tried >= 3 && hits >= 0.8 * tried)) {
      r.octave_shift = shift;
      r.midi_note = midi_note + shift;
      r.expected_hz = expected;
      f1 = fc;
      break;
    }
  }
  if (f1 <= 0) {  // no clear series: fall back to the named pitch
    f1 = spec.FindPeak(r.expected_hz * pow(2.0, -1.5 / 12), r.expected_hz * pow(2.0, 1.5 / 12), &level);
    if (f1 <= 0) return false;
  }
  double f0 = f1, B = 0;

  // Track partials outwards, refitting f0 and B as we go: (f_n / n)^2 = f0^2 (1 + B n^2).
  std::vector<Partial> found;
  std::vector<double> fit_x, fit_y, fit_w;
  for (int n = 1; n <= max_partials; ++n) {
    double predicted = n * f0 * sqrt(1 + B * n * n);
    if (predicted > 0.45 * sr) break;
    double half = std::min(0.25 * f0, predicted * 0.02);
    double lvl;
    double f = spec.FindPeak(predicted - half, predicted + half, &lvl);
    if (f <= 0 || lvl < r.noise_floor_db + 15) continue;
    Partial p = {};
    p.n = n; p.freq_hz = f; p.level_db = lvl; p.spec_db = lvl; p.t60_s = -1;
    found.push_back(p);
    fit_x.push_back(double(n) * n);
    fit_y.push_back((f / n) * (f / n));
    fit_w.push_back(pow(10.0, lvl / 20));
    double a, b;
    if (found.size() >= 4 && FitLine(fit_x, fit_y, fit_w, a, b) && a > 0) {
      f0 = sqrt(a);
      B = std::max(0.0, b / a);
    }
  }
  if (found.empty()) return false;
  r.f0_hz = f0;
  r.inharmonicity = B;
  r.tuning_cents = 1200 * log2(f0 / r.expected_hz);

  // Per-partial decay from band envelopes. Frames span >= 4 periods so neighbours don't leak in.
  const size_t frame = std::max<size_t>(1024, size_t(4.0 * sr / f0));
  const size_t hop = frame / 4;
  const double frame_dt = double(hop) / sr;
  for (Partial& p : found) {
    p.cents_off = 1200 * log2(p.freq_hz / (p.n * f0));
    std::vector<double> env = BandEnvelope(x, onset, sr, p.freq_hz, frame, hop);
    // Local noise: the band halfway to the next partial, late in the note.
    std::vector<double> noise = BandEnvelope(x, onset, sr, p.freq_hz + 0.5 * f0, frame, hop);
    if (env.size() < 8) continue;
    std::vector<double> nd(noise.begin() + noise.size() / 2, noise.end());
    std::sort(nd.begin(), nd.end());
    double floor_db = ToDb(nd.empty() ? 1e-9 : nd[nd.size() / 2]);
    // Start at the envelope peak within the first 150 ms, stop 50 dB down or 10 dB above noise.
    size_t start = 0, limit = std::min(env.size(), size_t(0.15 / frame_dt) + 1);
    for (size_t i = 0; i < limit; ++i) if (env[i] > env[start]) start = i;
    double top = ToDb(env[start]);
    double stop_db = std::max(top - 50.0, floor_db + 10.0);
    size_t end = start;
    while (end + 1 < env.size() && ToDb(env[end + 1]) > stop_db) ++end;
    if (end - start < 5) continue;
    std::vector<double> tx, ty, tw;
    for (size_t i = start; i <= end; ++i) { tx.push_back(i * frame_dt); ty.push_back(ToDb(env[i])); tw.push_back(1); }
    double a, b;
    if (!FitLine(tx, ty, tw, a, b) || b >= -0.01) continue;
    p.t60_s = -60.0 / b;
    p.level_db = a;  // fitted level at the onset
    p.fit_range_s = (end - start) * frame_dt;
    p.prompt_t60_s = p.after_t60_s = p.t60_s;
    p.knee_s = 0;
    p.after_db = -100;

    // Two-stage decay: best knee by least squares over two independent line segments.
    auto sse = [&](size_t lo, size_t hi, double& la, double& lb) {
      std::vector<double> x(tx.begin() + lo, tx.begin() + hi + 1), y(ty.begin() + lo, ty.begin() + hi + 1), w(x.size(), 1.0);
      if (!FitLine(x, y, w, la, lb)) return 1e300;
      double e = 0;
      for (size_t i = 0; i < x.size(); ++i) { double d = y[i] - (la + lb * x[i]); e += d * d; }
      return e;
    };
    double a1, b1line;
    const size_t m = tx.size() - 1;
    double single = sse(0, m, a1, b1line);
    double best = 1e300, pa = 0, pb = 0, aa = 0, ab = 0;
    size_t knee = 0;
    for (size_t k = 4; k + 4 <= m; ++k) {
      double la1, lb1, la2, lb2;
      double e = sse(0, k, la1, lb1) + sse(k, m, la2, lb2);
      if (e < best) { best = e; knee = k; pa = la1; pb = lb1; aa = la2; ab = lb2; }
    }
    if (knee > 0 && best < 0.6 * single && pb < -0.01 && ab < -0.01 && ab > pb) {
      p.prompt_t60_s = -60.0 / pb;
      p.after_t60_s = -60.0 / ab;
      p.knee_s = tx[knee] - tx[0];
      p.after_db = aa - pa;
    }
  }

  // Decay-rate model sigma(f) = b1 + b3 f^2. Every partial counts equally (weighting by level let the
  // single strongest partial decide); points far off the line (notches, beating) are dropped and
  // the line refitted.
  auto fit_sigma = [&](double Partial::*t60, double& out_b1, double& out_b3, bool two_stage_only = false) {
    std::vector<double> sx, sy, sw;
    for (const Partial& p : found) {
      if (p.*t60 <= 0) continue;
      if (two_stage_only && p.after_db <= -99) continue;  // single-slope partials say nothing here
      sx.push_back(p.freq_hz * p.freq_hz);
      sy.push_back(6.907755 / (p.*t60));
      sw.push_back(1.0);
    }
    if (sx.size() >= 2 && FitLine(sx, sy, sw, out_b1, out_b3) && sx.size() >= 6) {
      std::vector<double> res(sx.size());
      for (size_t i = 0; i < sx.size(); ++i) res[i] = fabs(sy[i] - (out_b1 + out_b3 * sx[i]));
      std::vector<double> sorted(res);
      std::sort(sorted.begin(), sorted.end());
      double mad = sorted[sorted.size() / 2] + 1e-9;
      for (size_t i = 0; i < sx.size(); ++i) sw[i] = res[i] > 3.0 * mad ? 0.0 : 1.0;
      FitLine(sx, sy, sw, out_b1, out_b3);
    }
  };
  fit_sigma(&Partial::t60_s, r.b1, r.b3);
  fit_sigma(&Partial::prompt_t60_s, r.prompt_b1, r.prompt_b3, true);
  fit_sigma(&Partial::after_t60_s, r.after_b1, r.after_b3, true);
  // Typical aftersound level and knee, over the partials that showed two stages (first 12).
  std::vector<double> levels, knees;
  for (const Partial& p : found)
    if (p.n <= 12 && p.after_db > -99) { levels.push_back(p.after_db); knees.push_back(p.knee_s); }
  if (!levels.empty()) {
    std::sort(levels.begin(), levels.end());
    std::sort(knees.begin(), knees.end());
    r.after_db = levels[levels.size() / 2];
    r.knee_s = knees[knees.size() / 2];
  }
  r.peak = peak;
  r.partials = found;
  return true;
}

int VelocityLayerFromFileName(const char* path) {
  const char* name = path;
  for (const char* p = path; *p; ++p) if (*p == '\\' || *p == '/') name = p + 1;
  for (const char* p = name; *p; ++p) {
    if (*p != '_' || p[1] != 'v') continue;
    const char* q = p + 2;
    if (*q == 'l') ++q;
    if (*q < '0' || *q > '9') continue;
    int v = 0;
    while (*q >= '0' && *q <= '9') v = v * 10 + (*q++ - '0');
    if (*q == '_' || *q == '.') return v;
  }
  return -1;
}

void PrintAnalysis(const NoteAnalysis& a) {
  static const char* names[] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
  if (a.octave_shift != 0)
    printf("(file name is %+d semitones off the recorded pitch; using the measured octave)\n", a.octave_shift);
  printf("note %s%d (MIDI %d): expected %.2f Hz, measured f0 %.3f Hz (%+.1f cents vs A440 ET), B = %.2e, noise floor %.0f dB\n",
         names[a.midi_note % 12], a.midi_note / 12 - 1, a.midi_note, a.expected_hz, a.f0_hz, a.tuning_cents,
         a.inharmonicity, a.noise_floor_db);
  printf("   n    freq Hz   cents   level dB    T60 s  (fit over s) | prompt  after  knee s  after dB\n");
  for (const Partial& p : a.partials) {
    if (p.t60_s > 0 && p.after_db > -99)
      printf("  %2d  %9.2f  %+6.1f  %8.1f  %7.2f  (%.2f)     | %6.2f %6.2f  %5.2f  %+6.1f\n", p.n, p.freq_hz, p.cents_off,
             p.level_db, p.t60_s, p.fit_range_s, p.prompt_t60_s, p.after_t60_s, p.knee_s, p.after_db);
    else if (p.t60_s > 0)
      printf("  %2d  %9.2f  %+6.1f  %8.1f  %7.2f  (%.2f)     | single stage\n", p.n, p.freq_hz, p.cents_off, p.level_db,
             p.t60_s, p.fit_range_s);
    else
      printf("  %2d  %9.2f  %+6.1f  %8.1f        -\n", p.n, p.freq_hz, p.cents_off, p.level_db);
  }
  if (a.after_db > -99)
    printf("two-stage decay: prompt T60 %.2f s, aftersound T60 %.2f s (at f0), aftersound %+.1f dB, knee %.2f s\n",
           6.907755 / std::max(1e-9, a.prompt_b1 + a.prompt_b3 * a.f0_hz * a.f0_hz),
           6.907755 / std::max(1e-9, a.after_b1 + a.after_b3 * a.f0_hz * a.f0_hz), a.after_db, a.knee_s);
  printf("decay model: sigma(f) = %.4f + %.3e f^2  ->  T60 %.2f s at f0, %.2f s at 2 kHz\n", a.b1, a.b3,
         6.907755 / std::max(1e-9, a.b1 + a.b3 * a.f0_hz * a.f0_hz),
         6.907755 / std::max(1e-9, a.b1 + a.b3 * 4e6));
}

}  // namespace analysis
