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

  //spc_player (legacy DirectSound path) is not wired up.

  SetClientSize(640,480);
  Center();
  Show();
  SetTimer(handle_, 1, 200, nullptr);  // refresh position in the title bar

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

void ClientMainWindow::UpdateTitle() {
  char text[MAX_PATH * 2];
  if (synth_player_ == nullptr || !synth_player_->output().running()) {
    bool lost = synth_player_ != nullptr && synth_player_->output().device_lost();
    sprintf_s(text, "%s - %s", title_, lost ? "audio device lost" : "no audio device");
  } else {
    static const char* state_names[] = {"Stopped", "Playing", "Paused"};
    auto& out = synth_player_->output();
    int pos = (int)synth_player_->GetPlaybackSeconds();
    int len = (int)(synth_player_->song_length_ms / 1000.0);
    sprintf_s(text, "%s - %s [%s %d:%02d / %d:%02d] - %u Hz, %.2f ms period - Space: play/pause, S: stop, O: open",
              title_, midi_name_[0] ? midi_name_ : "(no file)", state_names[synth_player_->state()],
              pos / 60, pos % 60, len / 60, len % 60, out.sample_rate(), out.period_ms());
  }
  SetWindowTextA(handle_, text);
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
        if (win->synth_player_ != nullptr) {
          if (wParam == VK_SPACE) {
            win->synth_player_->TogglePause();
          } else if (wParam == 'S') {
            win->synth_player_->Stop();
          } else if (wParam == 'O') {
            char path[MAX_PATH] = {};
            if (win->PromptForMidiFile(path, sizeof(path)))
              win->OpenMidi(path);
          }
          win->UpdateTitle();
        }
        return 0;
      case WM_TIMER:
        win->UpdateTitle();
        return 0;
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