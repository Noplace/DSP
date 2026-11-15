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

#include <functional>
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

  // files (client_main_window.cpp / studio_files.cpp)
  void BeforeDialog();
  bool PromptForFile(bool save, const char* filter, const char* default_ext, const char* title, char* path,
                     size_t path_size);
  void OpenMidi(const char* path, bool autoplay = true);
  void OpenAny(const char* path);  // MIDI file or session, by extension
  void PromptAndOpen();
  bool LoadSession(const char* path);
  bool SaveSession(const char* path);
  void PromptSaveSession(bool save_as);
  void PromptExport();
  void PromptSaveChain();
  void PromptLoadChain();
  void PromptExportMidi();
  void PollExport();
  void ShowMessage(const std::string& text, bool error = false);

  // editing, recording, loop and markers (studio_edit.cpp)
  void NewSong();
  void InstallEditedSong(dsp::audio::synth::SongInfo song, bool mark_modified = true);  // swap in, keeping the position
  void CommitSong(dsp::audio::synth::SongInfo song);          // undoable edit
  void Undo();
  void Redo();
  void RefreshTrackLists();
  int AddTrack();
  void DeleteTrack(int index);
  void StartRecording();
  void StopRecording();
  void PollRecording();
  void ToggleLoop();
  void AddMarker();
  double BeatMsAt(double ms, int* beats_per_bar) const;
  double SnapToBeat(double ms) const;
  // piano roll (piano_roll.cpp)
  void DrawPianoRoll(const Rect& r);
  void OpenEditor(int track);
  bool EditorKey(WPARAM key, bool ctrl, bool shift);  // true if the editor used the key
  void QuantizeSelection();
  void DeleteSelection();
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
  void DrawInstrumentPicker(const Rect& anchor);
  void DrawExportMenu(const Rect& anchor);
  // ruler with markers and the loop region; shared by the track view and the piano roll
  void DrawRuler(const Rect& ruler, double view_start_ms, double ms_per_px, const Rect& lanes);
  void DrawLoopAndPlayhead(const Rect& ruler, const Rect& lanes, double view_start_ms, double ms_per_px);
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
  std::string midi_path_;     // full path of the loaded file
  std::string session_path_;  // session last loaded/saved ("" = none)
  Exporter exporter_;
  std::string message_;       // transient status line (save/export results)
  bool message_error_ = false;
  double message_until_ = 0;
  bool picker_open_ = false;  // instrument picker popup
  int picker_family_ = 0;
  Rect picker_anchor_;
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
  std::vector<int> visible_tracks_;    // song tracks with notes, or made in the studio
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
  double inspector_wheel_until_ = 0;  // knobs ignore the wheel until then (panel scrolling)
  LevelMeter strip_meter_[16];
  LevelMeter master_meter_;
  float output_db_, limiter_db_;
  float output_hold_time_;
  std::vector<float> spectrum_db_;     // per graph pixel column
  int spectrum_source_;
  float analyzer_fft_[4096];

  // automation and MIDI learn (studio_automation.cpp)
  bool SlotToParam(const std::atomic<float>* slot, int* strip, int* param) const;
  std::string ParamLabel(int strip, int param) const;
  int FindLane(int strip, int param) const;
  void AddAutomationLane(int strip, int param);
  void RemoveAutomationLane(int strip, int param);
  void UpdateTouch();
  void EndTouch();
  void DrawParamMenu(const Rect& anchor);
  void DrawAutomationRow(const Rect& head, const Rect& lane, int lane_index, double view_start_ms, double ms_per_px);
  void RefreshMappedBits();
  bool param_menu_open_ = false;
  Rect param_menu_anchor_;
  int menu_strip_ = 0, menu_param_ = 0;
  bool touch_mode_ = false, touching_ = false;
  int touch_strip_ = 0, touch_param_ = -1;
  std::vector<std::pair<double, float>> touch_points_;
  int lane_drag_ = -1, lane_drag_point_ = -1;  // automation point being dragged
  std::vector<uint8_t> mapped_bits_;           // [strip + 1][param]: has a MIDI mapping

  // on-screen keyboard and mixer view options
  void DrawKeyboard(const Rect& r);
  int kb_note_ = -1;           // key held with the mouse
  bool mixer_used_only_ = false;
  bool mixer_wide_ = false;
  bool StripUsed(int ch) const;

  // metering / convolution
  dsp::audio::synth::effects::LoudnessStats loudness_stats_;
  float mb_gr_db_[3] = {0, 0, 0};
  std::string ir_path_;  // loaded impulse response file ("" = built-in preset)
  void PromptLoadIr();
  bool LoadIrFile(const std::string& path);

  // editing
  std::vector<dsp::audio::synth::SongInfo> undo_, redo_;
  bool song_modified_ = false;
  bool export_menu_open_ = false;
  Rect export_menu_anchor_;

  // transport extras
  bool count_in_ = false;
  std::atomic<float> speed_param_{1.0f};
  struct Marker {
    double ms;
    std::string name;
  };
  std::vector<Marker> markers_;
  int ruler_drag_ = 0;  // 0 none, 1 pressed (seek or loop), 2 new loop, 3 loop start, 4 loop end, 5 marker
  double ruler_anchor_ms_ = 0;
  float ruler_press_x_ = 0;
  int marker_drag_ = -1;

  // recording
  bool recording_ = false;
  struct TakeNote {
    double start_ms, end_ms;
    uint8_t note, velocity, channel;
  };
  std::vector<TakeNote> take_notes_;
  double take_open_[16][128];   // start ms of a held note, < 0 if none
  uint8_t take_velocity_[16][128];
  struct TakeEvent {
    double ms;
    uint32_t message;
  };
  std::vector<TakeEvent> take_events_;  // sustain pedal, other controllers, pitch bend
  int take_count_ = 0;

  // piano roll
  int editor_track_ = -1;  // track being edited, -1 = track view
  double ed_view_start_ms_ = 0, ed_ms_per_px_ = 10;
  float ed_key_h_ = 12;
  float ed_top_note_ = 84;  // note at the top edge
  int ed_snap_div_ = 16;    // grid 1/N notes, 0 = off
  bool ed_fit_pending_ = false;
  int ed_drag_ = 0;         // 0 none, 1 move, 2 resize, 3 rubber band, 4 velocity
  float ed_press_x_ = 0, ed_press_y_ = 0;
  int64_t ed_drag_ticks_ = 0;
  int ed_drag_notes_ = 0;
  int ed_grab_ = -1;        // note under the press
  uint64_t ed_last_length_ = 0;
  int ed_audition_note_ = -1, ed_audition_channel_ = 0;
  std::vector<char> ed_sel_;  // selection of the edited track's notes (UI only; saved into edits)
  int ed_sel_track_ = -1;
  uint64_t ed_sel_gen_ = 0;
  uint64_t song_gen_ = 1;     // bumped whenever a different/edited song is installed
  void SyncEditorSelection();
  void EditTrack(const std::function<void(dsp::audio::synth::SongTrack&, dsp::audio::synth::SongInfo&)>& fn);
  void Audition(int note, int channel, int velocity);  // -1 note = stop
};

}
