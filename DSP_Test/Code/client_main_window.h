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
namespace demo {
class ClientMainWindow {
 public:
  ClientMainWindow();
  ~ClientMainWindow();
  void Create();
  void Start();
  void Destroy();
  void SetClientSize(int width,int height);
  void Center();
  void Show();
  void Hide();
  void Fullscreen();
  void Windowed();
  void Step();
 private:
  bool PromptForMidiFile(char* path, size_t path_size);
  void OpenMidi(const char* path);
  void RescanMidiInputs();
  void UpdateTitle();
  void Paint();
  void OnTimer();
  void OnKeyDown(WPARAM key, LPARAM flags);
  void OnKeyUp(WPARAM key);
  void ReleaseHeldNotes();
  void SendLive(uint8_t status, uint8_t data1, uint8_t data2);

  dsp::audio::synth::RealtimePlayer* synth_player_;
  dsp::audio::synth::MidiSynth*      midi_synth_;
  dsp::audio::midi::MidiInput        midi_input_;
  // PC-keyboard piano (live channel 1)
  int octave_base_;          // MIDI note for the Z key
  int program_;              // GM program number 0..127
  uint8_t held_note_[256];   // per virtual key: note + 1 while held, 0 otherwise
  float level_db_;
  float limiter_db_;
  HFONT font_;
  dsp::audio::formats::SPCPlayer spc_player;
  char midi_name_[MAX_PATH];
  char title_[MAX_PATH];
  char class_name_[MAX_PATH];
  WNDCLASSEXA window_class_;
  HWND handle_;
  ATOM class_;
  HDC dc_;
  //LONG client_width_,client_height_;
  //GL gl;
  struct {
    DEVMODE mode;
    DWORD style;  
  } display[2];
  int dispmode;

  static LRESULT CALLBACK WindowProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam);
};
}
