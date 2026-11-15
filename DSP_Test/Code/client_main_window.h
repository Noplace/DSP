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
#pragma once

#include <vector>

namespace demo {

// The studio window: transport bar, track view (the MIDI file's tracks and notes), mixer
// (16 channel strips + master) and an inspector with the selected strip's processing chain.
// Drawn with Direct2D every frame; see studio_views.cpp for the panels.
class ClientMainWindow {
 public:
  ClientMainWindow();
  ~ClientMainWindow();
  void Create();
  void Start();
  void Destroy();

 private:
  struct LevelMeter {
    float db[2] = {-90, -90};
    float hold[2] = {-90, -90};
    float hold_time[2] = {0, 0};
    float gr_db = 0;  // gain reduction (<= 0)
    void Update(float peak_l, float peak_r, float gr_gain, float dt);
  };

  // window / input
  bool PromptForMidiFile(char* path, size_t path_size);
  void OpenMidi(const char* path);
  void PromptAndOpen();
  void RescanMidiInputs();
  void OnKeyDown(WPARAM key, LPARAM flags);
  void OnKeyUp(WPARAM key);
  void ReleaseHeldNotes();
  void SendLive(uint8_t status, uint8_t data1, uint8_t data2);
  void OnMouse(UINT msg, WPARAM wparam, LPARAM lparam);
  void SetInitialSize();
  static LRESULT CALLBACK WindowProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam);

  // frame
  void OnTimer();
  void Render();
  void UpdateMeters(float dt);
  void UpdateSpectrum();
  void DrawTopBar(const Rect& r);
  void DrawStatusBar(const Rect& r);
  void DrawTrackView(const Rect& r);
  void DrawMixer(const Rect& r);
  void DrawStrip(const Rect& r, int channel);
  void DrawMasterStrip(const Rect& r);
  void DrawMeter(const Rect& r, const LevelMeter& m);
  void DrawInspector(const Rect& r);
  float DrawChannelInspector(float x, float y, float w, int ch);
  float DrawMasterInspector(float x, float y, float w);
  void DrawResponseGraph(const Rect& r, int strip);  // strip -1 = master
  float DrawCard(float x, float y, float w, float body_h, const char* title, std::atomic<float>* power, Color accent);
  void DrawGainReduction(const Rect& r, float gr_db, const char* label = "GR");
  void Seek(double ms);
  void ZoomToFit();
  double PlayheadMs() const;
  const char* StripName(int channel) const;
  const char* ProgramName(int program) const;

  dsp::audio::synth::RealtimePlayer* synth_player_;
  dsp::audio::synth::MidiSynth* midi_synth_;
  dsp::audio::midi::MidiInput midi_input_;

  // PC-keyboard piano (live channel 1)
  int octave_base_;          // MIDI note for the Z key
  int program_;              // GM program number 0..127
  uint8_t held_note_[256];   // per virtual key: note + 1 while held, 0 otherwise

  char midi_name_[MAX_PATH];
  char title_[MAX_PATH];
  char class_name_[MAX_PATH];
  WNDCLASSEXA window_class_;
  HWND handle_;
  ATOM class_;

  Gfx gfx_;
  Ui ui_;
  double last_tick_, last_input_, last_active_, last_paint_;  // seconds (QPC)

  // studio state
  int selected_strip_;                 // -1 = master
  std::vector<int> visible_tracks_;    // file tracks that have notes
  std::string strip_names_[16];        // track name using each channel
  double view_start_ms_;               // track view: time at the left edge of the lanes
  double ms_per_px_;
  float lanes_w_;                      // width of the note lanes last frame
  float hbar_grab_;                    // horizontal scrollbar drag offset
  float track_scroll_y_;
  bool follow_playhead_;
  bool seek_dragging_;
  double seek_preview_ms_;
  float inspector_scroll_;
  float inspector_content_h_;
  float graph_h_ = 196;
  LevelMeter strip_meter_[16];
  LevelMeter master_meter_;
  float output_db_, limiter_db_;
  float output_hold_time_;
  std::vector<float> spectrum_db_;     // per graph pixel column
  int spectrum_source_;
  float analyzer_fft_[4096];
};

}
