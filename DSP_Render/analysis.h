// Analysis of recorded notes, for calibrating physical models against real instruments.
#pragma once

#include <stdint.h>
#include <string>
#include <vector>

namespace analysis {

struct Audio {
  uint32_t rate = 0;
  int channels = 0;
  std::vector<float> mono;   // channels averaged
};

// Reads PCM 16/24/32-bit and float32 WAV (any channel count). Returns false on failure.
bool ReadWav(const char* path, Audio& audio, std::string* error = nullptr);

// MIDI note from a name like "A#3", "C4", "Bb2" found in a file name (e.g. "Harpsi_Sus_A#3_rr1.wav").
// Returns -1 if none found.
int NoteFromFileName(const char* path);

struct Partial {
  int n;                 // partial number
  double freq_hz;        // measured
  double cents_off;      // vs n * f0 (positive = sharp)
  double level_db;       // initial level (fit intercept at the onset)
  double spec_db;        // level in the spectrum 30-770 ms after the onset
  double t60_s;          // decay time; < 0 if it could not be fitted
  double fit_range_s;    // length of the segment the decay was fitted over
  // Two-stage decay (piano unisons): fast "prompt" decay, then a slower "aftersound" from the knee
  // on. If one slope fits about as well, both equal t60_s and after_db is very low.
  double prompt_t60_s, after_t60_s;
  double knee_s;         // time of the change
  double after_db;       // aftersound line extrapolated to the onset, relative to the prompt line
};

// Velocity layer number from tags like "_vl3_" or "_v2_" in a file name, or -1.
int VelocityLayerFromFileName(const char* path);

struct NoteAnalysis {
  int midi_note = -1;        // as measured (named note + octave_shift)
  int octave_shift = 0;      // semitones added to the named note to match the recording
  double expected_hz = 0;
  double f0_hz = 0;          // fitted fundamental (from all partials, inharmonicity removed)
  double tuning_cents = 0;   // f0 vs equal temperament A440
  double inharmonicity = 0;  // B in f_n = n f0 sqrt(1 + B n^2)
  size_t onset = 0;          // sample index of the attack
  double noise_floor_db = 0;
  std::vector<Partial> partials;
  // Decay-rate model sigma(f) = b1 + b3 f^2 (1/s, amplitude e-folding rate), fitted to the
  // partials: T60 = ln(1000) / sigma.
  double b1 = 0, b3 = 0;
  // The same model for the prompt and aftersound stages, and the typical aftersound level
  // (median over partials, dB relative to the prompt at the onset) and knee time.
  double prompt_b1 = 0, prompt_b3 = 0, after_b1 = 0, after_b3 = 0;
  double after_db = -100, knee_s = 0;
  double peak = 0;           // peak sample level of the recording
};

// Analyses a sustained note. `midi_note` gives the expected pitch (the search allows +-1.5 semitones
// so historical tunings like A415 are found). The octave is checked automatically unless
// `forced_shift` (semitones added to midi_note) is given.
const int kAutoOctave = -1000;
bool AnalyzeNote(const Audio& audio, int midi_note, NoteAnalysis& result, int max_partials = 40,
                 int forced_shift = kAutoOctave);

void PrintAnalysis(const NoteAnalysis& a);

}  // namespace analysis
