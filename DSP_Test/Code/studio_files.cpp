// Files: opening MIDI files and sessions, saving sessions, chain presets and WAV export.
#include "global.h"

namespace demo {

using namespace dsp::audio::synth::effects;

namespace {

bool EndsWith(const std::string& s, const char* suffix) {
  const size_t n = strlen(suffix);
  if (s.size() < n) return false;
  return _stricmp(s.c_str() + s.size() - n, suffix) == 0;
}

std::string FileName(const std::string& path) {
  const size_t slash = path.find_last_of("\\/");
  return slash == std::string::npos ? path : path.substr(slash + 1);
}

std::string Directory(const std::string& path) {
  const size_t slash = path.find_last_of("\\/");
  return slash == std::string::npos ? std::string() : path.substr(0, slash);
}

std::string ReplaceExtension(const std::string& path, const char* ext) {
  const size_t dot = path.find_last_of('.');
  const size_t slash = path.find_last_of("\\/");
  if (dot == std::string::npos || (slash != std::string::npos && dot < slash)) return path + ext;
  return path.substr(0, dot) + ext;
}

bool FileExists(const std::string& path) {
  const DWORD attr = GetFileAttributesA(path.c_str());
  return attr != INVALID_FILE_ATTRIBUTES && !(attr & FILE_ATTRIBUTE_DIRECTORY);
}

std::string FullPath(const char* path) {
  char full[MAX_PATH];
  const DWORD n = GetFullPathNameA(path, MAX_PATH, full, nullptr);
  return n > 0 && n < MAX_PATH ? std::string(full) : std::string(path);
}

const double kMessageSeconds = 6.0;

}  // namespace

void ClientMainWindow::ShowMessage(const std::string& text, bool error) {
  message_ = text;
  message_error_ = error;
  LARGE_INTEGER now, freq;
  QueryPerformanceCounter(&now);
  QueryPerformanceFrequency(&freq);
  message_until_ = double(now.QuadPart) / freq.QuadPart + kMessageSeconds;
  InvalidateRect(handle_, nullptr, FALSE);
}

void ClientMainWindow::BeforeDialog() {
  ReleaseHeldNotes();
  ui_.in.down = false;  // the dialog swallows the button-up
  ui_.active = 0;
  picker_open_ = false;
}

bool ClientMainWindow::PromptForFile(bool save, const char* filter, const char* default_ext, const char* title,
                                     char* path, size_t path_size) {
  BeforeDialog();
  // IFileDialog rather than GetSaveFileName: the old dialog ignores the folder we suggest and
  // reopens whatever folder it remembers, while SetFolder() reliably starts where the song is.
  const std::string given = path;
  auto widen = [](const std::string& s) {
    std::wstring w(MultiByteToWideChar(CP_ACP, 0, s.c_str(), -1, nullptr, 0), L'\0');
    if (!w.empty()) MultiByteToWideChar(CP_ACP, 0, s.c_str(), -1, &w[0], int(w.size()));
    if (!w.empty()) w.pop_back();  // terminator
    return w;
  };
  // "desc\0pattern\0desc\0pattern\0\0" -> filter specs
  std::vector<std::wstring> strings;
  for (const char* p = filter; *p; p += strlen(p) + 1) strings.push_back(widen(p));
  std::vector<COMDLG_FILTERSPEC> specs;
  for (size_t i = 0; i + 1 < strings.size(); i += 2) specs.push_back({strings[i].c_str(), strings[i + 1].c_str()});

  const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
  bool ok = false;
  IFileDialog* dialog = nullptr;
  if (SUCCEEDED(CoCreateInstance(save ? CLSID_FileSaveDialog : CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
                                 IID_PPV_ARGS(&dialog)))) {
    DWORD options = 0;
    dialog->GetOptions(&options);
    dialog->SetOptions(options | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST |
                       (save ? FOS_OVERWRITEPROMPT : FOS_FILEMUSTEXIST));
    if (!specs.empty()) dialog->SetFileTypes(UINT(specs.size()), specs.data());
    if (default_ext) dialog->SetDefaultExtension(widen(default_ext).c_str());
    if (title) dialog->SetTitle(widen(title).c_str());
    if (!given.empty()) {
      if (given.find_first_of("\\/") != std::string::npos) {
        IShellItem* folder = nullptr;
        if (SUCCEEDED(SHCreateItemFromParsingName(widen(Directory(given)).c_str(), nullptr, IID_PPV_ARGS(&folder)))) {
          dialog->SetFolder(folder);
          folder->Release();
        }
      }
      if (save) dialog->SetFileName(widen(FileName(given)).c_str());
    }
    if (SUCCEEDED(dialog->Show(handle_))) {
      IShellItem* item = nullptr;
      if (SUCCEEDED(dialog->GetResult(&item))) {
        PWSTR chosen = nullptr;
        if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &chosen))) {
          ok = WideCharToMultiByte(CP_ACP, 0, chosen, -1, path, int(path_size), nullptr, nullptr) > 0;
          CoTaskMemFree(chosen);
        }
        item->Release();
      }
    }
    dialog->Release();
  }
  if (SUCCEEDED(com)) CoUninitialize();
  return ok;
}

void ClientMainWindow::PromptAndOpen() {
  char path[MAX_PATH] = {};
  if (PromptForFile(false,
                    "MIDI files and sessions (*.mid;*.midi;*.dspsession)\0*.mid;*.midi;*.dspsession\0"
                    "MIDI files (*.mid;*.midi)\0*.mid;*.midi\0Studio sessions (*.dspsession)\0*.dspsession\0"
                    "All files (*.*)\0*.*\0",
                    nullptr, "Open MIDI file or session", path, sizeof(path)))
    OpenAny(path);
}

void ClientMainWindow::OpenAny(const char* path) {
  if (EndsWith(path, ".dspsession")) LoadSession(path);
  else OpenMidi(path);
}

void ClientMainWindow::OpenMidi(const char* path, bool autoplay) {
  if (synth_player_ == nullptr || !synth_player_->output().running())
    return;
  const std::string full = FullPath(path);
  if (!FileExists(full)) {
    ShowMessage("File not found: " + full, true);
    return;
  }
  // Loading replaces the synth's track data, so keep the audio thread out while it happens.
  synth_player_->WithSynthLocked([&] { midi_synth_->LoadMidiFromFile(full.c_str()); });
  if (!midi_synth_->song().loaded || synth_player_->song_length_ms <= 0) {
    ShowMessage("Could not read " + full, true);
    return;
  }
  midi_path_ = full;
  session_path_.clear();  // a new song is not the old session any more
  strncpy_s(midi_name_, FileName(full).c_str(), _TRUNCATE);

  // Track view rows and strip names.
  const auto& song = midi_synth_->song();
  visible_tracks_.clear();
  for (auto& n : strip_names_) n.clear();
  for (int t = 0; t < int(song.tracks.size()); ++t) {
    const auto& track = song.tracks[t];
    if (track.notes.empty()) continue;
    visible_tracks_.push_back(t);
    for (int ch = 0; ch < 16; ++ch) {
      if (!(track.channel_mask & (1 << ch)) || !strip_names_[ch].empty()) continue;
      bool has_notes = false;
      for (auto& n : track.notes) if (n.channel == ch) { has_notes = true; break; }
      if (has_notes) strip_names_[ch] = track.name.empty() ? std::string("Track ") + std::to_string(t + 1) : track.name;
    }
  }
  track_scroll_y_ = 0;
  ZoomToFit();
  char title[MAX_PATH + 64];
  sprintf_s(title, "%s - %s", title_, midi_name_);
  SetWindowTextA(handle_, title);
  if (autoplay) synth_player_->Play();
}

// ---------------------------------------------------------------------------------------------
// Sessions
// ---------------------------------------------------------------------------------------------
bool ClientMainWindow::SaveSession(const char* path) {
  auto& mixer = midi_synth_->mixer;
  IniFile ini;
  auto& s = ini.Add("session");
  s.Set("version", 1);
  s.Set("midi", midi_path_);
  s.Set("selected_strip", selected_strip_ + 1);  // 0 = master
  s.Set("follow_file_sends", mixer.follow_midi_sends() ? 1 : 0);
  for (int ch = 0; ch < 16; ++ch) {
    IniFile::Section section{"channel " + std::to_string(ch + 1), {}};
    if (midi_synth_->channel_override(ch) >= 0) section.Set("instrument", midi_synth_->channel_override(ch));
    WriteStripParams(section, mixer.strip(ch), false);
    if (!section.values.empty()) ini.sections.push_back(section);
  }
  for (int t = 0; t < 64; ++t) {
    const bool mute = midi_synth_->track_muted(t), solo = midi_synth_->track_solo(t);
    if (!mute && !solo) continue;
    auto& section = ini.Add("track " + std::to_string(t + 1));
    if (mute) section.Set("mute", 1);
    if (solo) section.Set("solo", 1);
  }
  WriteMasterParams(ini.Add("master"), mixer.master());
  if (!ini.Save(path, "DSP Studio session")) {
    ShowMessage(std::string("Could not save ") + path, true);
    return false;
  }
  session_path_ = path;
  ShowMessage("Saved session " + FileName(path));
  return true;
}

bool ClientMainWindow::LoadSession(const char* path) {
  IniFile ini;
  if (!ini.Load(path) || !ini.Find("session")) {
    ShowMessage(std::string("Not a session file: ") + path, true);
    return false;
  }
  const auto& s = *ini.Find("session");
  auto& mixer = midi_synth_->mixer;

  // Mixer and instruments first: loading the song then starts from them.
  mixer.ResetAll();
  for (int ch = 0; ch < 16; ++ch) {
    const auto* section = ini.Find("channel " + std::to_string(ch + 1));
    midi_synth_->set_channel_override(ch, section ? std::clamp(int(section->GetNumber("instrument", -1)), -1, 128) : -1);
    if (section) ReadStripParams(*section, mixer.strip(ch), false);
  }
  if (auto* master = ini.Find("master")) ReadMasterParams(*master, mixer.master());
  mixer.set_follow_midi_sends(s.GetNumber("follow_file_sends", 1) != 0);

  // The MIDI file: as saved, else next to the session (moved folders).
  std::string midi = s.Get("midi") ? *s.Get("midi") : std::string();
  if (!midi.empty() && !FileExists(midi)) {
    const std::string beside = Directory(FullPath(path)) + "\\" + FileName(midi);
    if (FileExists(beside)) midi = beside;
  }
  bool loaded = false;
  if (!midi.empty() && FileExists(midi)) {
    OpenMidi(midi.c_str(), false);
    loaded = midi_path_ == FullPath(midi.c_str());
  }
  // Track mute/solo after loading (a load clears them).
  for (int t = 0; t < 64; ++t) {
    const auto* section = ini.Find("track " + std::to_string(t + 1));
    midi_synth_->set_track_muted(t, section && section->GetNumber("mute", 0) != 0);
    midi_synth_->set_track_solo(t, section && section->GetNumber("solo", 0) != 0);
  }
  selected_strip_ = std::clamp(int(s.GetNumber("selected_strip", 0)) - 1, -1, 15);
  session_path_ = FullPath(path);
  if (!midi.empty() && !loaded) ShowMessage("Session loaded, but its MIDI file was not found: " + midi, true);
  else ShowMessage("Loaded session " + FileName(path));
  return true;
}

void ClientMainWindow::PromptSaveSession(bool save_as) {
  if (!save_as && !session_path_.empty()) {
    SaveSession(session_path_.c_str());
    return;
  }
  char path[MAX_PATH] = {};
  const std::string suggested = !session_path_.empty() ? session_path_
                                : !midi_path_.empty()  ? ReplaceExtension(midi_path_, ".dspsession")
                                                       : std::string("Untitled.dspsession");
  strncpy_s(path, suggested.c_str(), _TRUNCATE);
  if (PromptForFile(true, "Studio sessions (*.dspsession)\0*.dspsession\0All files (*.*)\0*.*\0", "dspsession",
                    "Save session", path, sizeof(path)))
    SaveSession(path);
}

// ---------------------------------------------------------------------------------------------
// Chain presets: the selected strip's processing (.dspchain) or the master bus (.dspbus)
// ---------------------------------------------------------------------------------------------
void ClientMainWindow::PromptSaveChain() {
  const bool master = selected_strip_ < 0;
  char path[MAX_PATH] = {};
  strncpy_s(path, master ? "Master bus.dspbus" : "Chain.dspchain", _TRUNCATE);
  if (!PromptForFile(true,
                     master ? "Master bus presets (*.dspbus)\0*.dspbus\0All files (*.*)\0*.*\0"
                            : "Channel chain presets (*.dspchain)\0*.dspchain\0All files (*.*)\0*.*\0",
                     master ? "dspbus" : "dspchain", master ? "Save master bus preset" : "Save channel chain preset",
                     path, sizeof(path)))
    return;
  IniFile ini;
  if (master) WriteMasterParams(ini.Add("bus"), midi_synth_->mixer.master());
  else WriteStripParams(ini.Add("chain"), midi_synth_->mixer.strip(selected_strip_), true);
  if (ini.Save(path, master ? "DSP Studio master bus preset" : "DSP Studio channel chain preset"))
    ShowMessage("Saved preset " + FileName(path));
  else
    ShowMessage(std::string("Could not save ") + path, true);
}

void ClientMainWindow::PromptLoadChain() {
  const bool master = selected_strip_ < 0;
  char path[MAX_PATH] = {};
  if (!PromptForFile(false,
                     master ? "Master bus presets (*.dspbus)\0*.dspbus\0All files (*.*)\0*.*\0"
                            : "Channel chain presets (*.dspchain)\0*.dspchain\0All files (*.*)\0*.*\0",
                     nullptr, master ? "Load master bus preset" : "Load channel chain preset", path, sizeof(path)))
    return;
  IniFile ini;
  const auto* section = ini.Load(path) ? ini.Find(master ? "bus" : "chain") : nullptr;
  if (!section) {
    ShowMessage(std::string(master ? "Not a master bus preset: " : "Not a channel chain preset: ") + path, true);
    return;
  }
  if (master) ReadMasterParams(*section, midi_synth_->mixer.master());
  else ReadStripParams(*section, midi_synth_->mixer.strip(selected_strip_), true);
  ShowMessage("Loaded preset " + FileName(path));
}

// ---------------------------------------------------------------------------------------------
// WAV export
// ---------------------------------------------------------------------------------------------
void ClientMainWindow::PromptExport() {
  if (exporter_.busy()) return;
  if (midi_path_.empty()) {
    ShowMessage("Open a MIDI file first", true);
    return;
  }
  char path[MAX_PATH] = {};
  strncpy_s(path, ReplaceExtension(midi_path_, ".wav").c_str(), _TRUNCATE);
  if (!PromptForFile(true, "WAV audio (*.wav)\0*.wav\0", "wav", "Export WAV (24-bit)", path, sizeof(path)))
    return;
  // The settings are copied now: changes made while it renders do not affect this export.
  auto settings = std::make_unique<ExportSettings>();
  settings->CaptureFrom(*midi_synth_);
  settings->midi_path = midi_path_;
  settings->wav_path = path;
  settings->sample_rate = synth_player_->output().sample_rate() ? synth_player_->output().sample_rate() : 48000;
  exporter_.Start(*settings);
}

void ClientMainWindow::PollExport() {
  const auto state = exporter_.state();
  if (state == Exporter::kDone || state == Exporter::kFailed || state == Exporter::kCancelled) {
    ShowMessage(exporter_.result(), state == Exporter::kFailed);
    exporter_.Acknowledge();
  }
}

}
