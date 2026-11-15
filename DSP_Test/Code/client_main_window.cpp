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
#include <windowsx.h>

namespace demo {

namespace {

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

// In-place radix-2 FFT (n a power of two).
void Fft(float* re, float* im, int n) {
  for (int i = 1, j = 0; i < n; ++i) {
    int bit = n >> 1;
    for (; j & bit; bit >>= 1) j ^= bit;
    j ^= bit;
    if (i < j) { std::swap(re[i], re[j]); std::swap(im[i], im[j]); }
  }
  for (int len = 2; len <= n; len <<= 1) {
    const double ang = -2.0 * 3.14159265358979323846 / len;
    const float wr = float(cos(ang)), wi = float(sin(ang));
    for (int i = 0; i < n; i += len) {
      float cr = 1, ci = 0;
      for (int k = 0; k < len / 2; ++k) {
        const int a = i + k, b = a + len / 2;
        const float tr = re[b] * cr - im[b] * ci, ti = re[b] * ci + im[b] * cr;
        re[b] = re[a] - tr; im[b] = im[a] - ti;
        re[a] += tr; im[a] += ti;
        const float ncr = cr * wr - ci * wi;
        ci = cr * wi + ci * wr;
        cr = ncr;
      }
    }
  }
}

const int kFftSize = 4096;

double NowSeconds() {
  static LARGE_INTEGER freq = [] { LARGE_INTEGER f; QueryPerformanceFrequency(&f); return f; }();
  LARGE_INTEGER now;
  QueryPerformanceCounter(&now);
  return double(now.QuadPart) / freq.QuadPart;
}

}  // namespace

const char* ClientMainWindow::ProgramName(int program) const {
  if (program >= 128) return "Drum Kit";
  return GmProgramName(program);
}

ClientMainWindow::ClientMainWindow() : ui_(gfx_) {
  class_ = 0;
  handle_ = nullptr;
  synth_player_ = nullptr;
  midi_synth_ = nullptr;
  midi_name_[0] = 0;
  octave_base_ = 60;  // Z = middle C
  program_ = 0;
  memset(held_note_, 0, sizeof(held_note_));
  selected_strip_ = -1;
  view_start_ms_ = 0;
  ms_per_px_ = 50;
  lanes_w_ = 800;
  hbar_grab_ = 0;
  track_scroll_y_ = 0;
  follow_playhead_ = true;
  seek_dragging_ = false;
  seek_preview_ms_ = 0;
  inspector_scroll_ = 0;
  inspector_content_h_ = 0;
  output_db_ = -90;
  limiter_db_ = 0;
  output_hold_time_ = 0;
  spectrum_source_ = -2;
  last_tick_ = last_input_ = last_active_ = last_paint_ = 0;

  sprintf_s(title_, "DSP MIDI Synth");
  sprintf_s(class_name_, "%s_class", title_);
  memset(&window_class_, 0, sizeof(window_class_));
  window_class_.cbSize = sizeof(window_class_);
  window_class_.style = CS_HREDRAW | CS_VREDRAW | CS_DBLCLKS;
  window_class_.lpfnWndProc = WindowProc;
  window_class_.lpszClassName = class_name_;
  window_class_.hCursor = LoadCursor((HINSTANCE)NULL, IDC_ARROW);
  window_class_.hIcon = LoadIcon(nullptr, IDI_APPLICATION);
  window_class_.hbrBackground = nullptr;  // everything is drawn by Direct2D
}

ClientMainWindow::~ClientMainWindow() {
  Destroy();
}

void ClientMainWindow::Create() {
  class_ = RegisterClassEx(&window_class_);
  CreateWindowA(class_name_, title_, WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, CW_USEDEFAULT, CW_USEDEFAULT, 1280, 800,
                NULL, NULL, NULL, this);
  gfx_.Create(handle_);
  gfx_.SetDpi(float(GetDpiForWindow(handle_)));
}

void ClientMainWindow::SetInitialSize() {
  // 1600 x 960 DIPs, or most of the work area on smaller screens.
  const float scale = GetDpiForWindow(handle_) / 96.0f;
  MONITORINFO mi = {sizeof(mi)};
  GetMonitorInfo(MonitorFromWindow(handle_, MONITOR_DEFAULTTONEAREST), &mi);
  const int work_w = mi.rcWork.right - mi.rcWork.left, work_h = mi.rcWork.bottom - mi.rcWork.top;
  RECT rc = {0, 0, int(1600 * scale), int(960 * scale)};
  AdjustWindowRectExForDpi(&rc, WS_OVERLAPPEDWINDOW, FALSE, 0, GetDpiForWindow(handle_));
  int w = std::min<int>(rc.right - rc.left, int(work_w * 0.96));
  int h = std::min<int>(rc.bottom - rc.top, int(work_h * 0.96));
  SetWindowPos(handle_, nullptr, mi.rcWork.left + (work_w - w) / 2, mi.rcWork.top + (work_h - h) / 2, w, h,
               SWP_NOZORDER | SWP_NOACTIVATE);
}

void ClientMainWindow::Start() {
  midi_synth_ = new dsp::audio::synth::MidiSynth();
  midi_synth_->set_mode(dsp::audio::synth::MidiSynth::kModeSequencer);

  // Low-latency WASAPI output; the synth runs at the device's sample rate.
  synth_player_ = new dsp::audio::synth::RealtimePlayer();
  // Opening the device and building the instrument bank (calibrated piano and harpsichord
  // included) takes a few seconds: do it on a worker thread behind a splash window.
  bool audio_ok = false;
  {
    Splash splash;
    splash.Show("Opening the audio device and loading instruments...");
    splash.RunWhile([&] { audio_ok = synth_player_->Initialize(midi_synth_); });
  }
  if (!audio_ok) {
    MessageBoxA(handle_, synth_player_->last_error(), "Could not open audio device", MB_ICONERROR);
  }
  // In the studio, files get the reverb/chorus they ask for (CC91/CC93) unless switched off.
  midi_synth_->mixer.set_follow_midi_sends(true);
  RescanMidiInputs();

  SetInitialSize();
  ShowWindow(handle_, SW_SHOW);
  UpdateWindow(handle_);
  SetTimer(handle_, 1, 33, nullptr);  // 30 Hz tick (see OnTimer)

  // MIDI file from the command line; otherwise the track view offers an Open button.
  if (__argc > 1)
    OpenAny(__argv[1]);
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

void ClientMainWindow::Seek(double ms) {
  if (synth_player_ == nullptr || !midi_synth_->song().loaded)
    return;
  synth_player_->Seek(std::max(0.0, ms) / 1000.0);
}

double ClientMainWindow::PlayheadMs() const {
  if (seek_dragging_) return seek_preview_ms_;
  return synth_player_ ? synth_player_->GetPlaybackSeconds() * 1000.0 : 0.0;
}

void ClientMainWindow::ZoomToFit() {
  const double length = midi_synth_ ? midi_synth_->song().length_ms : 0;
  ms_per_px_ = length > 0 ? length / std::max(200.0f, lanes_w_ - 24) : 50.0;
  view_start_ms_ = 0;
}

const char* ClientMainWindow::StripName(int channel) const {
  return strip_names_[channel].c_str();
}

void ClientMainWindow::OnKeyDown(WPARAM key, LPARAM flags) {
  if (synth_player_ == nullptr)
    return;
  const bool repeat = (flags & (1 << 30)) != 0;
  const bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;

  const bool shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;

  if (EditorKey(key, ctrl, shift)) {
    // the piano roll used it (delete, select all, quantize, transpose, nudge)
  } else if (ctrl) {
    if (!repeat) {
      if (key == 'O') PromptAndOpen();
      else if (key == 'N') NewSong();
      else if (key == 'S') PromptSaveSession(shift);  // Ctrl+Shift+S = save as
      else if (key == 'E') PromptExport();
      else if (key == 'R') { if (recording_) StopRecording(); else StartRecording(); }
      else if (key == 'L') ToggleLoop();
      else if (key == 'M') AddMarker();
      else if (key == 'Z') { if (shift) Redo(); else Undo(); }
      else if (key == 'Y') Redo();
    }
  } else if (key == VK_SHIFT) {
    if (!repeat) SendLive(0xB0, 64, 127);  // sustain pedal down
  } else if (key == VK_SPACE) {
    if (!repeat) synth_player_->TogglePause();
  } else if (key == VK_ESCAPE) {
    if (picker_open_ || export_menu_open_) picker_open_ = export_menu_open_ = false;
    else synth_player_->Stop();
  } else if (key == VK_HOME) {
    Seek(0);
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
  last_input_ = NowSeconds();
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

void ClientMainWindow::OnMouse(UINT msg, WPARAM wparam, LPARAM lparam) {
  Input& in = ui_.in;
  const float scale = gfx_.scale();
  POINT pt = {GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
  if (msg == WM_MOUSEWHEEL) ScreenToClient(handle_, &pt);
  if (msg != WM_MOUSELEAVE) {
    in.mx = pt.x / scale;
    in.my = pt.y / scale;
  }
  in.shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
  in.ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
  switch (msg) {
    case WM_MOUSEMOVE: {
      TRACKMOUSEEVENT tme = {sizeof(tme), TME_LEAVE, handle_, 0};
      TrackMouseEvent(&tme);
      break;
    }
    case WM_MOUSELEAVE:
      if (!in.down) in.mx = in.my = -1000;
      break;
    case WM_LBUTTONDBLCLK:
      in.dbl = true;
      [[fallthrough]];
    case WM_LBUTTONDOWN:
      in.down = true;
      in.pressed = true;
      SetCapture(handle_);
      SetFocus(handle_);
      break;
    case WM_LBUTTONUP:
      in.down = false;
      in.released = true;
      ReleaseCapture();
      break;
    case WM_RBUTTONDOWN:
      in.rpressed = true;
      break;
    case WM_MOUSEWHEEL:
      in.wheel += GET_WHEEL_DELTA_WPARAM(wparam) / float(WHEEL_DELTA);
      break;
  }
  last_input_ = NowSeconds();
  InvalidateRect(handle_, nullptr, FALSE);
}

void ClientMainWindow::LevelMeter::Update(float peak_l, float peak_r, float gr_gain, float dt) {
  const float peaks[2] = {peak_l, peak_r};
  for (int c = 0; c < 2; ++c) {
    const float d = peaks[c] > 1e-5f ? 20.0f * log10f(peaks[c]) : -90.0f;
    db[c] = std::max(d, db[c] - 28.0f * dt);  // fall 28 dB/s
    if (d >= hold[c]) {
      hold[c] = d;
      hold_time[c] = 1.2f;
    } else if ((hold_time[c] -= dt) <= 0) {
      hold[c] = std::max(db[c], hold[c] - 20.0f * dt);
    }
  }
  const float g = gr_gain < 1.0f ? 20.0f * log10f(std::max(gr_gain, 1e-4f)) : 0.0f;
  gr_db = std::min(g, gr_db + 18.0f * dt);  // recover 18 dB/s on the display
}

void ClientMainWindow::UpdateMeters(float dt) {
  if (synth_player_ == nullptr || midi_synth_ == nullptr) return;
  auto& mixer = midi_synth_->mixer;
  for (int i = 0; i < 16; ++i) {
    float l, r;
    mixer.TakeStripPeak(i, l, r);
    strip_meter_[i].Update(l, r, mixer.TakeStripGainReduction(i), dt);
  }
  float l, r;
  mixer.TakeMasterPeak(l, r);
  master_meter_.Update(l, r, mixer.TakeMasterGainReduction(), dt);

  const float peak = synth_player_->TakePeakLevel();
  const float d = peak > 1e-5f ? 20.0f * log10f(peak) : -90.0f;
  output_db_ = std::max(d, output_db_ - 28.0f * dt);
  const float lim = 20.0f * log10f(std::max(synth_player_->TakeLimiterGain(), 1e-4f));
  limiter_db_ = std::min(lim, limiter_db_ + 18.0f * dt);
}

void ClientMainWindow::UpdateSpectrum() {
  auto& mixer = midi_synth_->mixer;
  mixer.set_analyzer_source(selected_strip_);
  const int bins = kFftSize / 2 + 1;
  if (spectrum_source_ != selected_strip_ || int(spectrum_db_.size()) != bins) {
    spectrum_db_.assign(bins, -120.0f);
    spectrum_source_ = selected_strip_;
  }
  static float re[kFftSize], im[kFftSize];
  mixer.ReadScope(re, kFftSize);
  float window_sum = 0;
  for (int i = 0; i < kFftSize; ++i) {
    const float w = 0.5f - 0.5f * cosf(6.2831853f * i / (kFftSize - 1));
    re[i] *= w;
    im[i] = 0;
    window_sum += w;
  }
  Fft(re, im, kFftSize);
  const float norm = 2.0f / window_sum;
  for (int b = 0; b < bins; ++b) {
    const float mag = sqrtf(re[b] * re[b] + im[b] * im[b]) * norm;
    const float db = mag > 1e-7f ? 20.0f * log10f(mag) : -140.0f;
    float& s = spectrum_db_[b];
    s = db > s ? s + (db - s) * 0.6f : s + (db - s) * 0.12f;  // fast rise, slow fall
  }
}


// 30 Hz tick: meters always advance; the window is only redrawn while something on it moves
// (playback, meters/analyser decaying, recent input) and about once a second otherwise.
// Drawing the whole studio costs several ms, so this keeps an idle window nearly free.
void ClientMainWindow::OnTimer() {
  const double now = NowSeconds();
  const float dt = last_tick_ > 0 ? float(std::min(0.1, now - last_tick_)) : 0.033f;
  last_tick_ = now;
  UpdateMeters(dt);
  bool active = seek_dragging_ || ui_.in.down || now - last_input_ < 1.0 || output_db_ > -89.0f ||
                limiter_db_ < -0.05f || master_meter_.hold[0] > -89.0f || master_meter_.hold[1] > -89.0f ||
                (synth_player_ && synth_player_->state() == dsp::audio::synth::RealtimePlayer::kStatePlaying);
  for (auto& m : strip_meter_)
    active = active || m.hold[0] > -89.0f || m.hold[1] > -89.0f || m.gr_db < -0.05f;
  PollExport();
  PollRecording();
  active = active || exporter_.busy() || recording_ || midi_synth_->counting_in() ||
           (!message_.empty() && now < message_until_ + 0.1);
  if (!message_.empty() && now > message_until_) message_.clear();
  if (active) last_active_ = now;
  // keep drawing a little after activity so the analyser and peak holds settle
  if (now - last_active_ < 2.0 || now - last_paint_ > 1.0)
    InvalidateRect(handle_, nullptr, FALSE);
}

void ClientMainWindow::Render() {
  if (IsIconic(handle_) || !gfx_.BeginFrame()) return;
  last_paint_ = NowSeconds();
  if (midi_synth_ != nullptr && synth_player_ != nullptr && synth_player_->output().running())
    UpdateSpectrum();
  ui_.BeginFrame();

  const float W = gfx_.width(), H = gfx_.height();
  gfx_.Fill(Rect(0, 0, W, H), theme::kBg);
  const float top_h = 56, status_h = 26;
  const float ins_w = W >= 1440 ? 480.0f : 440.0f;
  const float body_y = top_h, body_h = H - top_h - status_h;
  const float left_w = W - ins_w;
  // the piano roll gets a bit more height than the track view
  const float mixer_h = editor_track_ >= 0 ? std::clamp(body_h * 0.36f, 250.0f, 360.0f) : std::clamp(body_h * 0.42f, 290.0f, 380.0f);

  if (synth_player_ != nullptr && midi_synth_ != nullptr) {
    const bool popup = picker_open_ || export_menu_open_;
    ui_.SetModal(popup);  // the panels below are inert while a popup is up
    DrawTopBar(Rect(0, 0, W, top_h));
    if (editor_track_ >= 0) DrawPianoRoll(Rect(0, body_y, left_w, body_h - mixer_h));
    if (editor_track_ < 0) DrawTrackView(Rect(0, body_y, left_w, body_h - mixer_h));
    DrawMixer(Rect(0, body_y + body_h - mixer_h, left_w, mixer_h));
    DrawInspector(Rect(left_w, body_y, ins_w, body_h));
    if (popup) {
      ui_.SetModal(true);
      ui_.BeginModal();
      if (picker_open_) DrawInstrumentPicker(picker_anchor_);
      if (export_menu_open_) DrawExportMenu(export_menu_anchor_);
      ui_.EndModal();
    }
    ui_.SetModal(false);
    DrawStatusBar(Rect(0, H - status_h, W, status_h));
  }
  ui_.EndFrame();
  gfx_.EndFrame();
}

void ClientMainWindow::Destroy() {
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
  gfx_.Destroy();
  if (class_ != 0) {
    UnregisterClass(class_name_, 0);
    class_ = 0;
  }
}

LRESULT CALLBACK ClientMainWindow::WindowProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
  if (msg == WM_NCCREATE) {
    auto create_struct = reinterpret_cast<CREATESTRUCT*>(lparam);
    auto win = reinterpret_cast<ClientMainWindow*>(create_struct->lpCreateParams);
    win->handle_ = hwnd;
    SetWindowLongPtr(hwnd, GWLP_USERDATA, (LONG_PTR)win);
    return DefWindowProc(hwnd, msg, wparam, lparam);
  }
  auto win = reinterpret_cast<ClientMainWindow*>(GetWindowLongPtr(hwnd, GWLP_USERDATA));
  if (win != nullptr) {
    switch (msg) {
      case WM_KEYDOWN:
        win->OnKeyDown(wparam, lparam);
        return 0;
      case WM_KEYUP:
        win->OnKeyUp(wparam);
        return 0;
      case WM_SYSKEYDOWN:
        if (wparam == VK_F10) return 0;
        break;
      case WM_KILLFOCUS:
        win->ReleaseHeldNotes();  // key-ups go elsewhere once we lose focus
        return 0;
      case WM_MOUSEMOVE: case WM_LBUTTONDOWN: case WM_LBUTTONUP: case WM_LBUTTONDBLCLK:
      case WM_RBUTTONDOWN: case WM_MOUSEWHEEL: case WM_MOUSELEAVE:
        win->OnMouse(msg, wparam, lparam);
        return 0;
      case WM_CAPTURECHANGED:
        if ((HWND)lparam != hwnd && win->ui_.in.down) {
          win->ui_.in.down = false;
          win->ui_.in.released = true;
        }
        return 0;
      case WM_TIMER:
        win->OnTimer();
        return 0;
      case WM_PAINT:
        win->Render();
        ValidateRect(hwnd, nullptr);
        return 0;
      case WM_ERASEBKGND:
        return 1;  // Render() fills the background
      case WM_SIZE:
        win->gfx_.Resize();
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
      case WM_DPICHANGED: {
        const RECT* r = reinterpret_cast<RECT*>(lparam);
        win->gfx_.SetDpi(float(HIWORD(wparam)));
        SetWindowPos(hwnd, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
        return 0;
      }
      case WM_GETMINMAXINFO: {
        const float scale = GetDpiForWindow(hwnd) / 96.0f;
        auto info = reinterpret_cast<MINMAXINFO*>(lparam);
        info->ptMinTrackSize.x = LONG(1180 * scale);
        info->ptMinTrackSize.y = LONG(720 * scale);
        return 0;
      }
      case WM_SYSCOMMAND:
        if (wparam == SC_SCREENSAVE || wparam == SC_MONITORPOWER)
          return 0;
        break;
      case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
  }
  return DefWindowProc(hwnd, msg, wparam, lparam);
}

}
