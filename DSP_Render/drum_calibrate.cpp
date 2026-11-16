// Builds a drum calibration (DrumCalibration, drum_calibration.h) from recorded hits.
//
//   DSP_Render drumcal <manifest.txt> <out.cal>
//
// Manifest lines (paths relative to the current folder, * wildcards allowed):
//   name <text>
//   note <midi> <residual_ms> <max_modes> <min_hz> <max_hz> <gain_db> <file pattern> [<file pattern> ...]
//   derive <midi> <from_midi> <semitones> <gain_db>
//
// Each recording is decomposed into
//  1. its ringing modes: spectral peaks of the tail (from 20 ms after the hit), each one's decay
//     measured frame by frame, and their complex amplitudes at the hit fitted jointly by least
//     squares over the next 0.5 s;
//  2. the residual: the recording minus those modes for its first residual_ms, faded out over
//     the last 30%. It holds the stick/beater contact, wires, noise and room.
// Velocities come from the takes' levels (the kit's loudness curve, ~32 dB from velocity 127 to
// 30), and each note is scaled so its loudest take sits at -20 dBFS (50 ms RMS) + gain_db.
#define NOMINMAX
#include <algorithm>
#include <complex>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <string>
#include <vector>
#include "../DSP_Lib/Code/dsp.h"
#include "analysis.h"

using dsp::audio::synth::instruments::DrumCalibration;
using dsp::audio::synth::instruments::DrumCalMode;
using dsp::audio::synth::instruments::DrumCalNote;
using dsp::audio::synth::instruments::DrumCalTake;

namespace {

const double kPi2 = 6.283185307179586;

void FftInPlace(std::vector<double>& re, std::vector<double>& im) {
  const size_t n = re.size();
  for (size_t i = 1, j = 0; i < n; ++i) {
    size_t bit = n >> 1;
    for (; j & bit; bit >>= 1) j ^= bit;
    j ^= bit;
    if (i < j) { std::swap(re[i], re[j]); std::swap(im[i], im[j]); }
  }
  for (size_t len = 2; len <= n; len <<= 1) {
    const double a = -kPi2 / len;
    const double wr = cos(a), wi = sin(a);
    for (size_t i = 0; i < n; i += len) {
      double cr = 1, ci = 0;
      for (size_t k = 0; k < len / 2; ++k) {
        const size_t u = i + k, v = i + k + len / 2;
        const double xr = re[v] * cr - im[v] * ci, xi = re[v] * ci + im[v] * cr;
        re[v] = re[u] - xr; im[v] = im[u] - xi;
        re[u] += xr; im[u] += xi;
        const double t = cr * wr - ci * wi;
        ci = cr * wi + ci * wr;
        cr = t;
      }
    }
  }
}

double Loudest50ms(const std::vector<float>& x, uint32_t rate) {
  const size_t win = rate / 20;
  double best = 0;
  for (size_t s = 0; s + win <= x.size(); s += win / 4) {
    double sum = 0;
    for (size_t i = s; i < s + win; ++i) sum += double(x[i]) * x[i];
    best = std::max(best, sum / win);
  }
  return sqrt(best);
}

std::vector<std::string> Glob(const std::string& pattern) {
  std::vector<std::string> out;
  const size_t slash = pattern.find_last_of("\\/");
  const std::string dir = slash == std::string::npos ? "" : pattern.substr(0, slash + 1);
  WIN32_FIND_DATAA fd;
  HANDLE h = FindFirstFileA(pattern.c_str(), &fd);
  if (h == INVALID_HANDLE_VALUE) return out;
  do {
    if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) out.push_back(dir + fd.cFileName);
  } while (FindNextFileA(h, &fd));
  FindClose(h);
  std::sort(out.begin(), out.end());
  return out;
}

struct NoteSpec {
  int midi = 0;
  double residual_ms = 300, min_hz = 30, max_hz = 8000, gain_db = 0;
  int max_modes = 48;
};

struct FitStats { int modes = 0; double explained_db = 0; double low_hz = 0; };

// Solves A z = b (n x n, row major) by Gaussian elimination with partial pivoting.
bool Solve(std::vector<double>& a, std::vector<double>& b, int n) {
  for (int c = 0; c < n; ++c) {
    int p = c;
    for (int r = c + 1; r < n; ++r) if (fabs(a[r * n + c]) > fabs(a[p * n + c])) p = r;
    if (fabs(a[p * n + c]) < 1e-30) return false;
    if (p != c) {
      for (int k = 0; k < n; ++k) std::swap(a[c * n + k], a[p * n + k]);
      std::swap(b[c], b[p]);
    }
    for (int r = c + 1; r < n; ++r) {
      const double f = a[r * n + c] / a[c * n + c];
      if (f == 0) continue;
      for (int k = c; k < n; ++k) a[r * n + k] -= f * a[c * n + k];
      b[r] -= f * b[c];
    }
  }
  for (int c = n - 1; c >= 0; --c) {
    double s = b[c];
    for (int k = c + 1; k < n; ++k) s -= a[c * n + k] * b[k];
    b[c] = s / a[c * n + c];
  }
  return true;
}

bool AnalyzeTake(const std::vector<float>& rec, uint32_t rate, const NoteSpec& spec, DrumCalTake& take,
                 std::vector<float>& residual, FitStats& stats) {
  float peak = 0;
  for (float v : rec) peak = std::max(peak, fabsf(v));
  if (peak <= 0) return false;
  size_t onset = 0;
  while (onset < rec.size() && fabsf(rec[onset]) < 0.1f * peak) ++onset;
  const size_t pre = rate / 500;  // keep 2 ms before the hit
  const size_t start = onset > pre ? onset - pre : 0;
  const size_t total = std::min(rec.size() - start, size_t(std::max(spec.residual_ms / 1000.0, 3.0) * rate));
  std::vector<float> x(rec.begin() + start, rec.begin() + start + total);
  const size_t ja = (onset - start) + rate / 50;  // tail analysis from 20 ms after the hit
  take.level = float(Loudest50ms(x, rate));

  // 1. Spectral peaks of the tail.
  std::vector<DrumCalMode> modes;
  std::vector<double> measured;  // each mode's amplitude in the first tail frame (centre at ja + frame / 2)
  const size_t frame = rate / 11, hop = frame / 2;
  if (ja + rate / 20 < total) {
    const size_t w = std::min(total - ja, size_t(0.6 * rate));
    size_t n = 1;
    while (n < 4 * w && n < (1u << 19)) n <<= 1;
    std::vector<double> re(n, 0.0), im(n, 0.0);
    for (size_t i = 0; i < w; ++i) re[i] = x[ja + i] * (0.5 - 0.5 * cos(kPi2 * i / (w - 1)));
    FftInPlace(re, im);
    const size_t bins = n / 2;
    std::vector<double> db(bins);
    double top = -300;
    for (size_t k = 0; k < bins; ++k) {
      db[k] = 10 * log10(re[k] * re[k] + im[k] * im[k] + 1e-30);
      top = std::max(top, db[k]);
    }
    const size_t kmin = std::max<size_t>(3, size_t(spec.min_hz * n / rate));
    const size_t kmax = std::min(bins - 4, size_t(std::min(spec.max_hz, 0.45 * rate) * n / rate));
    const int half = int(std::max<size_t>(8, n / 2048));  // floor estimate: +-~20 Hz
    struct Peak { double level, bin; };
    std::vector<Peak> peaks;
    std::vector<double> neighbourhood;
    for (size_t k = kmin; k < kmax; ++k) {
      bool is_max = true;
      for (int d = -3; d <= 3 && is_max; ++d) if (d && db[k + d] > db[k]) is_max = false;
      if (!is_max || db[k] < top - 60) continue;
      neighbourhood.clear();
      for (int d = -half * 4; d <= half * 4; d += 2) {
        const long long q = (long long)k + d;
        if (q >= 0 && q < (long long)bins) neighbourhood.push_back(db[size_t(q)]);
      }
      std::nth_element(neighbourhood.begin(), neighbourhood.begin() + neighbourhood.size() / 4, neighbourhood.end());
      if (db[k] < neighbourhood[neighbourhood.size() / 4] + 10) continue;
      const double a = db[k - 1], b = db[k], c = db[k + 1];
      const double den = a - 2 * b + c;
      peaks.push_back({b, k + (den < 0 ? 0.5 * (a - c) / den : 0.0)});
    }
    std::sort(peaks.begin(), peaks.end(), [](const Peak& a, const Peak& b) { return a.level > b.level; });

    // 2. Decay of each peak, frame by frame (Hann frames of ~93 ms, half overlap).
    for (const Peak& pk : peaks) {
      if (int(modes.size()) >= spec.max_modes) break;
      const double f = pk.bin * rate / n;
      std::vector<double> fdb;
      double first_amp = 0;
      for (size_t s = ja; s + frame <= total && fdb.size() < 60; s += hop) {
        double sr = 0, si = 0;
        const double dw = kPi2 * f / rate;
        for (size_t i = 0; i < frame; ++i) {
          const double win = 0.5 - 0.5 * cos(kPi2 * i / (frame - 1));
          sr += x[s + i] * win * cos(dw * i);
          si -= x[s + i] * win * sin(dw * i);
        }
        if (fdb.empty()) first_amp = 2 * sqrt(sr * sr + si * si) / (0.5 * (frame - 1));
        fdb.push_back(10 * log10(sr * sr + si * si + 1e-30));
      }
      if (fdb.size() < 3) continue;
      // Fit while the level falls (stop at the noise floor or when beating turns it back up).
      size_t m = 1;
      double lowest = fdb[0];
      while (m < fdb.size() && fdb[m] > fdb[0] - 45 && fdb[m] < lowest + 6) { lowest = std::min(lowest, fdb[m]); ++m; }
      if (m < 3) m = std::min<size_t>(3, fdb.size());
      double st = 0, sy = 0, stt = 0, sty = 0;
      for (size_t i = 0; i < m; ++i) {
        const double t = i * double(hop) / rate;
        st += t; sy += fdb[i]; stt += t * t; sty += t * fdb[i];
      }
      const double slope = (m * sty - st * sy) / std::max(1e-12, m * stt - st * st);  // dB per second
      double t60 = slope < -0.3 ? -60.0 / slope : 20.0;
      t60 = std::min(20.0, t60);
      if (t60 < 0.06) continue;  // dies within the attack: left to the residual
      DrumCalMode mode;
      mode.freq_hz = float(f);
      mode.t60_s = float(t60);
      modes.push_back(mode);
      measured.push_back(first_amp);
    }
  }

  // 3. Complex amplitudes at the hit: y[j] = sum Im(z_k p_k^(j+1)), least squares over the tail.
  const int k_count = int(modes.size());
  std::vector<std::complex<double>> p(k_count), pj(k_count);
  for (int k = 0; k < k_count; ++k)
    p[k] = std::polar(exp(-6.907755 / (modes[k].t60_s * rate)), kPi2 * modes[k].freq_hz / rate);
  if (k_count > 0) {
    const int u = 2 * k_count;
    const size_t j0 = ja, j1 = std::min(total, ja + size_t(0.5 * rate));
    std::vector<double> g(size_t(u) * u, 0.0), b(u, 0.0), phi(u);
    for (int k = 0; k < k_count; ++k) pj[k] = std::pow(p[k], double(j0 + 1));
    for (size_t j = j0; j < j1; ++j) {
      for (int k = 0; k < k_count; ++k) {
        phi[2 * k] = pj[k].imag();      // coefficient of Re z
        phi[2 * k + 1] = pj[k].real();  // coefficient of Im z
        pj[k] *= p[k];
      }
      const double y = x[j];
      for (int r = 0; r < u; ++r) {
        b[r] += phi[r] * y;
        double* row = &g[size_t(r) * u];
        const double pr = phi[r];
        for (int c = r; c < u; ++c) row[c] += pr * phi[c];
      }
    }
    double trace = 0;
    for (int r = 0; r < u; ++r) {
      for (int c = 0; c < r; ++c) g[size_t(r) * u + c] = g[size_t(c) * u + r];
      trace += g[size_t(r) * u + r];
    }
    for (int r = 0; r < u; ++r) g[size_t(r) * u + r] += 1e-7 * trace / u;
    if (!Solve(g, b, u)) return false;
    for (int k = 0; k < k_count; ++k) { modes[k].re = float(b[2 * k]); modes[k].im = float(b[2 * k + 1]); }
    // Close modes can come out of a joint fit with large amplitudes of opposite sign that cancel
    // over the fit window but not later (different decays): cap each at what the spectrum shows.
    const double tc = double(ja + frame / 2);
    for (int k = 0; k < k_count; ++k) {
      const double a = std::abs(std::complex<double>(modes[k].re, modes[k].im)) * std::pow(std::abs(p[k]), tc + 1);
      if (a > 1.5 * measured[k] && a > 0) {
        const float shrink = float(1.5 * measured[k] / a);
        modes[k].re *= shrink;
        modes[k].im *= shrink;
      }
    }
    // The modes alone must not ring louder than the recording later in the tail (a decay measured
    // too long, or room sound counted as ringing): in each late window, shorten the decay of the
    // mode that rings loudest there until the modes sit at or below the recording.
    const double windows[][2] = {{0.3, 0.7}, {0.7, 1.5}, {1.5, 3.0}};
    for (const auto& win : windows) {
      const size_t la = ja + size_t(win[0] * rate), lb = std::min(total, ja + size_t(win[1] * rate));
      if (lb < la + rate / 10) break;
      double ex = 1e-30;
      for (size_t j = la; j < lb; ++j) ex += double(x[j]) * x[j];
      auto late = [&](int k) {
        const double r2 = std::norm(p[k]);
        const double z2 = std::norm(std::complex<double>(modes[k].re, modes[k].im));
        return 0.5 * z2 * std::pow(r2, double(la + 1)) * (1 - std::pow(r2, double(lb - la))) / std::max(1e-12, 1 - r2);
      };
      for (int iteration = 0; iteration < 400; ++iteration) {
        double em = 0, worst = 0;
        int worst_k = -1;
        for (int k = 0; k < k_count; ++k) {
          const double e = late(k);
          em += e;
          if (e > worst) { worst = e; worst_k = k; }
        }
        if (em <= 1.25 * ex || worst_k < 0) break;
        modes[worst_k].t60_s *= 0.85f;
        p[worst_k] = std::polar(exp(-6.907755 / (modes[worst_k].t60_s * rate)), kPi2 * modes[worst_k].freq_hz / rate);
      }
    }
  }

  // 4. Residual = recording - modes over the first residual_ms, faded out at the end.
  const size_t r_len = std::min(total, size_t(spec.residual_ms / 1000.0 * rate));
  residual.assign(r_len, 0.0f);
  std::vector<double> model(total, 0.0);
  for (int k = 0; k < k_count; ++k) {
    std::complex<double> z(modes[k].re, modes[k].im);
    for (size_t j = 0; j < total; ++j) {
      z *= p[k];
      model[j] += z.imag();
    }
  }
  const size_t fade = r_len * 3 / 10;
  for (size_t j = 0; j < r_len; ++j) {
    double w = 1;
    if (j >= r_len - fade) w = 0.5 + 0.5 * cos(3.14159265358979 * double(j - (r_len - fade)) / fade);
    residual[j] = float((x[j] - model[j]) * w);
  }
  // How much of the tail the modes explain (20 ms .. 0.5 s after the hit).
  double ex = 1e-30, er = 1e-30;
  for (size_t j = ja; j < std::min(total, ja + size_t(0.5 * rate)); ++j) {
    ex += double(x[j]) * x[j];
    er += (x[j] - model[j]) * (x[j] - model[j]);
  }
  stats.modes = k_count;
  stats.explained_db = 10 * log10(ex / er);
  double strongest = 0;
  stats.low_hz = 0;
  for (const DrumCalMode& m : modes) {
    const double a = std::abs(std::complex<double>(m.re, m.im));
    if (m.freq_hz < 1000 && a > strongest) { strongest = a; stats.low_hz = m.freq_hz; }
  }
  // Drop pruned modes and ones too weak to matter (> 50 dB below the take's strongest).
  double strongest_amp = 0;
  for (const DrumCalMode& m : modes) strongest_amp = std::max(strongest_amp, std::abs(std::complex<double>(m.re, m.im)));
  modes.erase(std::remove_if(modes.begin(), modes.end(), [&](const DrumCalMode& m) {
                return std::abs(std::complex<double>(m.re, m.im)) < strongest_amp * 0.00316;
              }), modes.end());
  take.modes = std::move(modes);
  return true;
}

}  // namespace

// Lists what a drum calibration holds for a note: each take's velocity, residual and modes.
int CmdDrumCalInfo(int argc, char** argv) {
  if (argc < 3) return 2;
  DrumCalibration cal;
  if (!cal.Load(argv[2])) { fprintf(stderr, "cannot read %s\n", argv[2]); return 1; }
  printf("%s: %zu notes\n", cal.name.c_str(), cal.notes.size());
  const int only = argc > 3 ? atoi(argv[3]) : -1;
  for (const DrumCalNote& n : cal.notes) {
    if (only >= 0 && n.midi_note != only) continue;
    if (n.from_note >= 0) { printf("note %d: from note %d, %+.1f semitones, %+.1f dB\n", n.midi_note, n.from_note, n.semitones, 20 * log10(n.gain)); continue; }
    printf("note %d: %zu takes at %u Hz\n", n.midi_note, n.takes.size(), n.rate);
    for (const DrumCalTake& t : n.takes) {
      printf("  %-36s vel %3.0f  level %5.1f dB  residual %4.0f ms  %zu modes\n", t.source.c_str(), t.velocity * 127,
             20 * log10(t.level + 1e-12), t.residual.size() * 1000.0 / n.rate, t.modes.size());
      if (only < 0) continue;
      for (const DrumCalMode& m : t.modes)
        printf("      %8.1f Hz  T60 %6.2f s  amp %6.1f dB\n", m.freq_hz, m.t60_s,
               20 * log10(std::abs(std::complex<double>(m.re, m.im)) + 1e-12));
    }
  }
  return 0;
}

int CmdDrumCalibrate(int argc, char** argv) {
  if (argc < 4) return 2;
  FILE* f = nullptr;
  if (fopen_s(&f, argv[2], "r") != 0 || !f) { fprintf(stderr, "cannot read %s\n", argv[2]); return 1; }
  DrumCalibration cal;
  char line[2048];
  printf("note  take                                   vel  modes  explained  strongest<1k  residual\n");
  while (fgets(line, sizeof(line), f)) {
    std::vector<std::string> tok;
    for (char* t = strtok(line, " \t\r\n"); t; t = strtok(nullptr, " \t\r\n")) tok.push_back(t);
    if (tok.empty() || tok[0][0] == '#') continue;
    if (tok[0] == "name") {
      cal.name.clear();
      for (size_t i = 1; i < tok.size(); ++i) cal.name += (i > 1 ? " " : "") + tok[i];
    } else if (tok[0] == "derive" && tok.size() >= 5) {
      DrumCalNote n;
      n.midi_note = atoi(tok[1].c_str());
      n.from_note = atoi(tok[2].c_str());
      n.semitones = float(atof(tok[3].c_str()));
      n.gain = float(pow(10.0, atof(tok[4].c_str()) / 20));
      cal.notes.push_back(n);
    } else if (tok[0] == "note" && tok.size() >= 8) {
      NoteSpec spec;
      spec.midi = atoi(tok[1].c_str());
      spec.residual_ms = atof(tok[2].c_str());
      spec.max_modes = atoi(tok[3].c_str());
      spec.min_hz = atof(tok[4].c_str());
      spec.max_hz = atof(tok[5].c_str());
      spec.gain_db = atof(tok[6].c_str());
      DrumCalNote n;
      n.midi_note = spec.midi;
      std::vector<std::vector<float>> residuals;
      for (size_t i = 7; i < tok.size(); ++i) {
        for (const std::string& path : Glob(tok[i])) {
          analysis::Audio audio;
          std::string error;
          if (!analysis::ReadWav(path.c_str(), audio, &error)) { fprintf(stderr, "%s: %s\n", path.c_str(), error.c_str()); continue; }
          if (n.rate && audio.rate != n.rate) { fprintf(stderr, "%s: sample rate differs, skipped\n", path.c_str()); continue; }
          n.rate = audio.rate;
          DrumCalTake take;
          std::vector<float> residual;
          FitStats stats;
          if (!AnalyzeTake(audio.mono, audio.rate, spec, take, residual, stats)) { fprintf(stderr, "%s: analysis failed\n", path.c_str()); continue; }
          const size_t slash = path.find_last_of("\\/");
          take.source = path.substr(slash == std::string::npos ? 0 : slash + 1);
          printf("%4d  %-38s   -   %4d   %6.1f dB   %8.1f Hz   %5.0f ms\n", spec.midi, take.source.c_str(), stats.modes,
                 stats.explained_db, stats.low_hz, residual.size() * 1000.0 / audio.rate);
          n.takes.push_back(std::move(take));
          residuals.push_back(std::move(residual));
        }
      }
      if (n.takes.empty()) { fprintf(stderr, "note %d: no recordings found\n", spec.midi); continue; }
      // Velocity from level; the note's loudest take at -20 dBFS + gain_db.
      float loudest = 0;
      for (const DrumCalTake& t : n.takes) loudest = std::max(loudest, t.level);
      const float scale = float(pow(10.0, (-20.0 + spec.gain_db) / 20.0)) / std::max(1e-9f, loudest);
      std::vector<size_t> order(n.takes.size());
      for (size_t i = 0; i < order.size(); ++i) order[i] = i;
      for (size_t i = 0; i < n.takes.size(); ++i) {
        DrumCalTake& t = n.takes[i];
        t.velocity = float(std::clamp(pow(10.0, 20 * log10(std::max(1e-9f, t.level) / loudest) / 32.0), 0.05, 1.0));
        t.level *= scale;
        for (DrumCalMode& m : t.modes) { m.re *= scale; m.im *= scale; }
        float rmax = 1e-9f;
        for (float v : residuals[i]) rmax = std::max(rmax, fabsf(v) * scale);
        t.residual_scale = rmax / 32767.0f;
        t.residual.resize(residuals[i].size());
        for (size_t j = 0; j < residuals[i].size(); ++j)
          t.residual[j] = int16_t(std::clamp(lrintf(residuals[i][j] * scale / t.residual_scale), -32767L, 32767L));
      }
      std::sort(order.begin(), order.end(), [&](size_t a, size_t b) { return n.takes[a].velocity < n.takes[b].velocity; });
      std::vector<DrumCalTake> sorted;
      for (size_t i : order) sorted.push_back(std::move(n.takes[i]));
      n.takes = std::move(sorted);
      printf("      velocities:");
      for (const DrumCalTake& t : n.takes) printf(" %.0f", t.velocity * 127);
      printf("\n");
      cal.notes.push_back(std::move(n));
    }
  }
  fclose(f);
  if (cal.notes.empty()) { fprintf(stderr, "nothing to write\n"); return 1; }
  if (!cal.Save(argv[3])) { fprintf(stderr, "cannot write %s\n", argv[3]); return 1; }
  size_t takes = 0, samples = 0, modes = 0;
  for (const DrumCalNote& n : cal.notes)
    for (const DrumCalTake& t : n.takes) { ++takes; samples += t.residual.size(); modes += t.modes.size(); }
  printf("wrote %s: %zu notes, %zu recorded hits, %zu modes, %.1f s of residual\n", argv[3], cal.notes.size(), takes,
         modes, samples / 44100.0);
  return 0;
}
