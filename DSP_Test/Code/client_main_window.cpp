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
#include "global.h"

namespace demo {

ClientMainWindow::ClientMainWindow() {
  dispmode = 0;
  class_ = 0;
  synth_player_ = nullptr;
  midi_synth_ = nullptr;
  midi_name_[0] = 0;
  octave_base_ = 60;  // Z = middle C
  program_ = 0;
  memset(held_note_, 0, sizeof(held_note_));
  level_db_ = -120.0f;
  limiter_db_ = 0.0f;
  font_ = nullptr;
  memset(display,0,sizeof(display));
  display[0].style = WS_VISIBLE | WS_BORDER|WS_CAPTION|WS_MINIMIZEBOX|WS_SYSMENU|WS_CLIPSIBLINGS | WS_CLIPCHILDREN;
  display[1].style = WS_POPUP | WS_VISIBLE | WS_MAXIMIZE;


  memset(&display[0].mode, 0, sizeof(display[0].mode));
	display[0].mode.dmSize       = sizeof(display[0].mode);
	display[0].mode.dmPelsWidth  = (unsigned long)640;
	display[0].mode.dmPelsHeight = (unsigned long)480;
	display[0].mode.dmBitsPerPel = 32;			
	display[0].mode.dmFields     = DM_BITSPERPEL | DM_PELSWIDTH | DM_PELSHEIGHT;
 
  memset(&display[1].mode, 0, sizeof(display[1].mode));
	display[1].mode.dmSize       = sizeof(display[1].mode);
	display[1].mode.dmPelsWidth  = (unsigned long)640;
	display[1].mode.dmPelsHeight = (unsigned long)480;
	display[1].mode.dmBitsPerPel = 32;			
	display[1].mode.dmFields     = DM_BITSPERPEL | DM_PELSWIDTH | DM_PELSHEIGHT;

 

  sprintf_s(title_,"DSP MIDI Synth");
  sprintf_s(class_name_,"%s_class",title_);
  memset(&window_class_,0,sizeof(window_class_));
  window_class_.cbSize = sizeof(window_class_);
  window_class_.cbClsExtra = 0;
  window_class_.cbWndExtra = 0;
  window_class_.style = CS_HREDRAW | CS_VREDRAW | CS_OWNDC;
  window_class_.lpfnWndProc = WindowProc;
  window_class_.lpszClassName = class_name_;
  window_class_.hCursor = LoadCursor((HINSTANCE) NULL, IDC_ARROW); 
  window_class_.hbrBackground = static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)); 
}

ClientMainWindow::~ClientMainWindow() {
  Destroy();
}

void ClientMainWindow::Create() {
  class_ = RegisterClassEx(&window_class_);
  CreateWindow(class_name_,title_,display[dispmode].style,0,0,100,100,NULL,NULL,NULL,this);
  SetWindowText(handle_,title_);
  dc_ = GetDC(handle_);
  /*PIXELFORMATDESCRIPTOR pfd = {
    0,1,PFD_SUPPORT_OPENGL|PFD_DOUBLEBUFFER, 32, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 32, 0, 0, 0, 0, 0, 0, 0
  };
  SetPixelFormat(dc_, ChoosePixelFormat(dc_, &pfd) , &pfd);
  */

  
}

void ClientMainWindow::Start() {
  midi_synth_ = new dsp::audio::synth::MidiSynth();
  midi_synth_->set_mode(dsp::audio::synth::MidiSynth::kModeSequencer);
  //midi_synth_->delay_unit.set_delay_ms(300.0f);
  //midi_synth_->delay_unit.set_feedback(0.4f);
  //midi_synth_->main_effects[0] = &midi_synth_->delay_unit;

  // Low-latency WASAPI output; the synth runs at the device's sample rate.
  synth_player_ = new dsp::audio::synth::RealtimePlayer();
  if (!synth_player_->Initialize(midi_synth_)) {
    MessageBoxA(handle_, synth_player_->last_error(), "Could not open audio device", MB_ICONERROR);
  }
  RescanMidiInputs();

  //spc_player (legacy DirectSound path) is not wired up.

  font_ = CreateFontA(-16, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                      CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, FIXED_PITCH | FF_MODERN, "Consolas");
  SetClientSize(760,300);
  Center();
  Show();
  SetTimer(handle_, 1, 200, nullptr);  // refresh status (position, level)

  // MIDI file: first command-line argument, otherwise ask with an Open dialog.
  char midi_path[MAX_PATH] = {};
  if (__argc > 1)
    strncpy_s(midi_path, __argv[1], _TRUNCATE);
  else if (!PromptForMidiFile(midi_path, sizeof(midi_path)))
    midi_path[0] = 0;
  if (midi_path[0] != 0)
    OpenMidi(midi_path);
  UpdateTitle();
}

bool ClientMainWindow::PromptForMidiFile(char* path, size_t path_size) {
  OPENFILENAMEA ofn = {};
  ofn.lStructSize = sizeof(ofn);
  ofn.hwndOwner = handle_;
  ofn.lpstrFilter = "MIDI files (*.mid;*.midi)\0*.mid;*.midi\0All files (*.*)\0*.*\0";
  ofn.lpstrFile = path;
  ofn.nMaxFile = (DWORD)path_size;
  ofn.lpstrTitle = "Open MIDI file";
  ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
  return GetOpenFileNameA(&ofn) != FALSE;
}

void ClientMainWindow::OpenMidi(const char* path) {
  if (synth_player_ == nullptr)
    return;
  // Loading replaces the synth's track data, so keep the audio thread out while it happens.
  synth_player_->WithSynthLocked([&] { midi_synth_->LoadMidiFromFile(path); });
  const char* name = strrchr(path, '\\');
  strncpy_s(midi_name_, name ? name + 1 : path, _TRUNCATE);
  synth_player_->Play();
}

void ClientMainWindow::RescanMidiInputs() {
  if (synth_player_ == nullptr)
    return;
  ReleaseHeldNotes();
  // Ports are opened/closed while the audio thread is kept out of the synth.
  synth_player_->WithSynthLocked([&] {
    midi_input_.OpenAll();
    synth_player_->set_midi_input(&midi_input_);
  });
}

void ClientMainWindow::SendLive(uint8_t status, uint8_t data1, uint8_t data2) {
  if (synth_player_ != nullptr)
    synth_player_->PostMidiMessage(status, data1, data2);
}

void ClientMainWindow::ReleaseHeldNotes() {
  SendLive(0xB0, 64, 0);  // sustain pedal up as well
  for (int vk = 0; vk < 256; ++vk) {
    if (held_note_[vk]) {
      SendLive(0x80, uint8_t(held_note_[vk] - 1), 64);
      held_note_[vk] = 0;
    }
  }
}

namespace {

// Tracker-style layout: bottom two rows are one octave from octave_base_, top two rows the next.
int PianoKeyOffset(WPARAM vk) {
  switch (vk) {
    case 'Z': return 0;  case 'S': return 1;  case 'X': return 2;  case 'D': return 3;
    case 'C': return 4;  case 'V': return 5;  case 'G': return 6;  case 'B': return 7;
    case 'H': return 8;  case 'N': return 9;  case 'J': return 10; case 'M': return 11;
    case VK_OEM_COMMA: return 12; case 'L': return 13; case VK_OEM_PERIOD: return 14;
    case 'Q': return 12; case '2': return 13; case 'W': return 14; case '3': return 15;
    case 'E': return 16; case 'R': return 17; case '5': return 18; case 'T': return 19;
    case '6': return 20; case 'Y': return 21; case '7': return 22; case 'U': return 23;
    case 'I': return 24; case '9': return 25; case 'O': return 26; case '0': return 27;
    case 'P': return 28;
  }
  return -1;
}

const char* NoteName(int note) {
  static const char* names[] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
  return names[note % 12];
}

const char* GmProgramName(int program) {
  static const char* names[128] = {
    "Acoustic Grand Piano", "Bright Acoustic Piano", "Electric Grand Piano", "Honky-tonk Piano",
    "Electric Piano 1", "Electric Piano 2", "Harpsichord", "Clavinet",
    "Celesta", "Glockenspiel", "Music Box", "Vibraphone", "Marimba", "Xylophone", "Tubular Bells", "Dulcimer",
    "Drawbar Organ", "Percussive Organ", "Rock Organ", "Church Organ", "Reed Organ", "Accordion", "Harmonica",
    "Tango Accordion",
    "Nylon Guitar", "Steel Guitar", "Jazz Guitar", "Clean Guitar", "Muted Guitar", "Overdriven Guitar",
    "Distortion Guitar", "Guitar Harmonics",
    "Acoustic Bass", "Fingered Bass", "Picked Bass", "Fretless Bass", "Slap Bass 1", "Slap Bass 2",
    "Synth Bass 1", "Synth Bass 2",
    "Violin", "Viola", "Cello", "Contrabass", "Tremolo Strings", "Pizzicato Strings", "Orchestral Harp", "Timpani",
    "String Ensemble 1", "String Ensemble 2", "Synth Strings 1", "Synth Strings 2", "Choir Aahs", "Voice Oohs",
    "Synth Voice", "Orchestra Hit",
    "Trumpet", "Trombone", "Tuba", "Muted Trumpet", "French Horn", "Brass Section", "Synth Brass 1", "Synth Brass 2",
    "Soprano Sax", "Alto Sax", "Tenor Sax", "Baritone Sax", "Oboe", "English Horn", "Bassoon", "Clarinet",
    "Piccolo", "Flute", "Recorder", "Pan Flute", "Blown Bottle", "Shakuhachi", "Whistle", "Ocarina",
    "Square Lead", "Saw Lead", "Calliope Lead", "Chiff Lead", "Charang Lead", "Voice Lead", "Fifths Lead",
    "Bass + Lead",
    "New Age Pad", "Warm Pad", "Polysynth Pad", "Choir Pad", "Bowed Pad", "Metallic Pad", "Halo Pad", "Sweep Pad",
    "Rain FX", "Soundtrack FX", "Crystal FX", "Atmosphere FX", "Brightness FX", "Goblins FX", "Echoes FX",
    "Sci-fi FX",
    "Sitar", "Banjo", "Shamisen", "Koto", "Kalimba", "Bagpipe", "Fiddle", "Shanai",
    "Tinkle Bell", "Agogo", "Steel Drums", "Woodblock", "Taiko Drum", "Melodic Tom", "Synth Drum", "Reverse Cymbal",
    "Guitar Fret Noise", "Breath Noise", "Seashore", "Bird Tweet", "Telephone Ring", "Helicopter", "Applause",
    "Gunshot"};
  return names[program & 127];
}

}  // namespace

void ClientMainWindow::OnKeyDown(WPARAM key, LPARAM flags) {
  if (synth_player_ == nullptr)
    return;
  const bool repeat = (flags & (1 << 30)) != 0;
  const bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;

  if (ctrl) {
    if (key == 'O' && !repeat) {
      ReleaseHeldNotes();
      char path[MAX_PATH] = {};
      if (PromptForMidiFile(path, sizeof(path)))
        OpenMidi(path);
    }
  } else if (key == VK_SHIFT) {
    if (!repeat) SendLive(0xB0, 64, 127);  // sustain pedal down
  } else if (key == VK_SPACE) {
    if (!repeat) synth_player_->TogglePause();
  } else if (key == VK_ESCAPE) {
    synth_player_->Stop();
  } else if (key == VK_F5) {
    if (!repeat) RescanMidiInputs();
  } else if (key == VK_UP || key == VK_DOWN) {
    ReleaseHeldNotes();
    octave_base_ += key == VK_UP ? 12 : -12;
    if (octave_base_ < 24) octave_base_ = 24;
    if (octave_base_ > 96) octave_base_ = 96;
  } else if (key == VK_LEFT || key == VK_RIGHT) {
    program_ = (program_ + (key == VK_RIGHT ? 1 : 127)) % 128;
    SendLive(0xC0, uint8_t(program_), 0);
  } else if (!repeat && key < 256 && held_note_[key] == 0) {
    int offset = PianoKeyOffset(key);
    if (offset >= 0) {
      int note = octave_base_ + offset;
      if (note <= 127) {
        held_note_[key] = uint8_t(note + 1);
        SendLive(0x90, uint8_t(note), 100);
      }
    }
  }
  UpdateTitle();
  InvalidateRect(handle_, nullptr, FALSE);
}

void ClientMainWindow::OnKeyUp(WPARAM key) {
  if (key == VK_SHIFT) {
    SendLive(0xB0, 64, 0);  // sustain pedal up
    return;
  }
  if (key < 256 && held_note_[key]) {
    SendLive(0x80, uint8_t(held_note_[key] - 1), 64);
    held_note_[key] = 0;
  }
}

void ClientMainWindow::OnTimer() {
  if (synth_player_ != nullptr) {
    float peak = synth_player_->TakePeakLevel();
    level_db_ = peak > 0.000001f ? 20.0f * log10f(peak) : -120.0f;
    limiter_db_ = 20.0f * log10f(synth_player_->TakeLimiterGain());
  }
  UpdateTitle();
  InvalidateRect(handle_, nullptr, FALSE);
}

void ClientMainWindow::UpdateTitle() {
  char text[MAX_PATH * 2];
  if (synth_player_ == nullptr || !synth_player_->output().running()) {
    bool lost = synth_player_ != nullptr && synth_player_->output().device_lost();
    sprintf_s(text, "%s - %s", title_, lost ? "audio device lost" : "no audio device");
  } else {
    static const char* state_names[] = {"Stopped", "Playing", "Paused"};
    int pos = (int)synth_player_->GetPlaybackSeconds();
    int len = (int)(synth_player_->song_length_ms / 1000.0);
    sprintf_s(text, "%s - %s [%s %d:%02d / %d:%02d] level %.0f dB limiter %.1f dB", title_,
              midi_name_[0] ? midi_name_ : "(no file)", state_names[synth_player_->state()],
              pos / 60, pos % 60, len / 60, len % 60, level_db_, limiter_db_);
  }
  SetWindowTextA(handle_, text);
}

void ClientMainWindow::Paint() {
  PAINTSTRUCT ps;
  HDC dc = BeginPaint(handle_, &ps);
  RECT client;
  GetClientRect(handle_, &client);
  FillRect(dc, &client, static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));
  HGDIOBJ old_font = SelectObject(dc, font_ ? font_ : GetStockObject(ANSI_FIXED_FONT));
  SetBkMode(dc, TRANSPARENT);

  char lines[12][256] = {};
  int n = 0;
  if (synth_player_ != nullptr && synth_player_->output().running()) {
    static const char* state_names[] = {"Stopped", "Playing", "Paused"};
    auto& out = synth_player_->output();
    int pos = (int)synth_player_->GetPlaybackSeconds();
    int len = (int)(synth_player_->song_length_ms / 1000.0);
    sprintf_s(lines[n++], "File    %s   %s %d:%02d / %d:%02d", midi_name_[0] ? midi_name_ : "(none)",
              state_names[synth_player_->state()], pos / 60, pos % 60, len / 60, len % 60);
    char level[32];
    if (level_db_ <= -100.0f) strcpy_s(level, "-inf"); else sprintf_s(level, "%.1f", level_db_);
    sprintf_s(lines[n++], "Audio   WASAPI %u Hz, %.2f ms period (%s)", out.sample_rate(), out.period_ms(),
              out.low_latency_mode() ? "low-latency" : "default period");
    sprintf_s(lines[n++], "Output  level %s dB   limiter %.1f dB", level, limiter_db_);
  } else {
    sprintf_s(lines[n++], "Audio   no audio device");
  }
  if (midi_input_.open_count() == 0) {
    sprintf_s(lines[n++], "MIDI in none open (%d port(s) found) - connect a keyboard and press F5",
              midi_input_.available_count());
  } else {
    int len = sprintf_s(lines[n], "MIDI in %d of %d:", midi_input_.open_count(), midi_input_.available_count());
    for (int i = 0; i < midi_input_.open_count() && len < 200; ++i)
      len += sprintf_s(lines[n] + len, sizeof(lines[n]) - len, " [%s]", midi_input_.port_name(i));
    ++n;
  }
  sprintf_s(lines[n++], "PC keys Z = %s%d, program %d %s (channel 1)", NoteName(octave_base_), octave_base_ / 12 - 1,
            program_ + 1, GmProgramName(program_));
  const int status_lines = n++;  // bright status lines, then a gap, then dimmer help
  sprintf_s(lines[n++], "Space play/pause file   Esc stop   Ctrl+O open file   F5 rescan MIDI ports");
  sprintf_s(lines[n++], "Z S X D C V G B H N J M ,   and   Q 2 W 3 E R 5 T 6 Y 7 U I   play notes");
  sprintf_s(lines[n++], "Up/Down octave   Left/Right program   Shift sustain pedal");

  int y = 12;
  for (int i = 0; i < n; ++i) {
    SetTextColor(dc, i < status_lines ? RGB(230, 230, 230) : RGB(150, 150, 150));
    TextOutA(dc, 14, y, lines[i], (int)strlen(lines[i]));
    y += 22;
  }
  SelectObject(dc, old_font);
  EndPaint(handle_, &ps);
}

void ClientMainWindow::Destroy() {
  //gl.Deinitialize(handle_);
  if (synth_player_ != nullptr) {
    KillTimer(handle_, 1);
    delete synth_player_;  // stops the audio thread before the synth goes away
    synth_player_ = nullptr;
  }
  if (midi_synth_ != nullptr) {
    delete midi_synth_;
    midi_synth_ = nullptr;
  }
  midi_input_.CloseAll();  // audio thread is gone, nothing drains the ports any more
  if (font_ != nullptr) {
    DeleteObject(font_);
    font_ = nullptr;
  }
  if (class_!=0) {
    UnregisterClass(class_name_,0);
    class_ = 0;
  }
  //ChangeDisplaySettings(0,0);
}



void ClientMainWindow::SetClientSize(int width,int height) {
  RECT rectangle;
  SetRect(&rectangle,0,0,width,height);
  AdjustWindowRectEx(&rectangle,display[dispmode].style,0,0);
  int actual_width  = static_cast<int>(rectangle.right  - rectangle.left);
  int actual_height = static_cast<int>(rectangle.bottom - rectangle.top);
  SetWindowPos(handle_,0,0,0,actual_width,actual_height,SWP_NOACTIVATE|SWP_NOMOVE);  
}

void ClientMainWindow::Center() {
  RECT rectangle;
  GetWindowRect(handle_,&rectangle);
  
  int screen_width  = GetSystemMetrics(SM_CXSCREEN);
  int screen_height = GetSystemMetrics(SM_CYSCREEN);
  
  int left   = (screen_width - rectangle.right)/2;
  int top    = (screen_height - rectangle.bottom)/2;
  
  SetWindowPos(handle_,0,left,top,0,0,SWP_NOACTIVATE|SWP_NOSIZE);  
  
}

void ClientMainWindow::Show() {
  ShowWindow(handle_, SW_SHOW); 
  UpdateWindow(handle_); 
}

void ClientMainWindow::Hide() {
  ShowWindow(handle_, SW_HIDE); 
}

void ClientMainWindow::Fullscreen() {
  return;
  dispmode = 1;
  display[dispmode].mode.dmPelsWidth = 1024;
  display[dispmode].mode.dmPelsHeight = 768;

  SetWindowPos(handle_,0,0,0,0,0,SWP_NOACTIVATE|SWP_NOSIZE); 
  SetWindowLongPtr(handle_,GWL_STYLE,display[dispmode].style);
  SetClientSize(display[dispmode].mode.dmPelsWidth,display[dispmode].mode.dmPelsHeight);

  auto r = ChangeDisplaySettings(&display[dispmode].mode,CDS_FULLSCREEN);
  ShowCursor(0);
  
    //HDC hDC = GetDC(CreateWindow("edit",0,WS_POPUP | WS_VISIBLE | WS_MAXIMIZE, 0, 0, 0, 0, 0, 0, 0, 0));
  //wglMakeCurrent(hDC, wglCreateContext(hDC));
  //HDC hDC = GetDC(CreateWindow("edit",0,WS_POPUP | WS_VISIBLE | WS_MAXIMIZE, 0, 0, 0, 0, 0, 0, 0, 0));
  //SetPixelFormat(hDC, ChoosePixelFormat(hDC, &pfd) , &pfd);
  //wglMakeCurrent(hDC, wglCreateContext(hDC));
  //
}

void ClientMainWindow::Windowed() {
  dispmode = 0;
  display[dispmode].mode.dmPelsWidth = 640;
  display[dispmode].mode.dmPelsHeight = 480;

  //ChangeDisplaySettings(0,0);
  ShowCursor(1);

  SetWindowLongPtr(handle_,GWL_STYLE,display[dispmode].style);
  SetClientSize(display[dispmode].mode.dmPelsWidth,display[dispmode].mode.dmPelsHeight);
  Center();


}

void ClientMainWindow::Step() {
  //gl.BeginScene(1,1,0,1);

  //gl.EndScene();
}

LRESULT CALLBACK ClientMainWindow::WindowProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
  static ClientMainWindow* win = nullptr;
 
  switch( uMsg ) {
    case WM_NCCREATE: {
      CREATESTRUCT* create_struct = reinterpret_cast<CREATESTRUCT*>(lParam);
      win = reinterpret_cast<ClientMainWindow*>(create_struct->lpCreateParams);
      win->handle_ = hwnd;
      SetWindowLongPtr(hwnd,GWLP_USERDATA,(ULONG_PTR)(win));
      return TRUE;
    }  
  }
  
  //ClientMainWindow* current_window = reinterpret_cast<ClientMainWindow*>(static_cast<LONG_PTR>(GetWindowLongPtr(hwnd,GWLP_USERDATA)));
  
  if (win != nullptr) {
    switch( uMsg ) {
      case WM_KEYDOWN:
        win->OnKeyDown(wParam, lParam);
        return 0;
      case WM_KEYUP:
        win->OnKeyUp(wParam);
        return 0;
      case WM_KILLFOCUS:
        win->ReleaseHeldNotes();  // key-ups go elsewhere once we lose focus
        return 0;
      case WM_TIMER:
        win->OnTimer();
        return 0;
      case WM_PAINT:
        win->Paint();
        return 0;
      case WM_ERASEBKGND:
        return 1;  // Paint() fills the background
      case WM_SYSCOMMAND:
        if (wParam == SC_SCREENSAVE || wParam == SC_MONITORPOWER)
          return 0;
      break;
      case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
  }
  return DefWindowProc(hwnd,uMsg,wParam,lParam);
}

}