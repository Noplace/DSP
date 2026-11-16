// Files: opening MIDI files and sessions, saving sessions, chain presets and WAV export.
#include "global.h"

namespace demo {

using namespace dsp::audio::synth::effects;

namespace {

// Strip names in session files: "master", "channel 1".."channel 16", "drums kick".."drums fx".
std::string StripTarget(int strip) {
  if (strip < 0) return "master";
  if (Mixer::IsKitStrip(strip)) {
    std::string name = dsp::audio::synth::DrumGroupName(strip - Mixer::kKitFirst);
    for (auto& c : name) c = char(tolower(c));
    return "drums " + name;
  }
  return "channel " + std::to_string(strip + 1);
}

// -1 master, 0..kAllStrips-1, or -2 when the name isn't a strip.
int ParseStripTarget(const std::string& target) {
  if (target == "master") return -1;
  if (target.rfind("channel ", 0) == 0) {
    const int c = atoi(target.c_str() + 8);
    return c >= 1 && c <= Mixer::kStrips ? c - 1 : -2;
  }
  for (int s = Mixer::kKitFirst; s < Mixer::kAllStrips; ++s)
    if (StripTarget(s) == target) return s;
  return -2;
}

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
  if (recording_) StopRecording();
  // Loading replaces the synth's track data, so keep the audio thread out while it happens.
  synth_player_->WithSynthLocked([&] { midi_synth_->LoadMidiFromFile(full.c_str()); });
  if (!midi_synth_->song().loaded || synth_player_->song_length_ms <= 0) {
    ShowMessage("Could not read " + full, true);
    return;
  }
  midi_path_ = full;
  session_path_.clear();  // a new song is not the old session any more
  strncpy_s(midi_name_, FileName(full).c_str(), _TRUNCATE);
  undo_.clear();
  loudness_stats_.Reset();
  redo_.clear();
  markers_.clear();
  midi_synth_->set_loop(false, 0, 0);
  song_modified_ = false;
  ++song_gen_;
  editor_track_ = -1;
  RefreshTrackLists();
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
  // An edited or new song goes into its own MIDI file next to the session; the original file
  // is never overwritten.
  const auto& song = midi_synth_->song();
  if (song.loaded && (song_modified_ || midi_path_.empty()) && song.TotalNotes() > 0) {
    const std::string song_path = ReplaceExtension(FullPath(path), ".song.mid");
    if (!SaveSongAsMidi(song, song_path.c_str())) {
      ShowMessage("Could not write the song to " + song_path, true);
      return false;
    }
    midi_path_ = song_path;
    strncpy_s(midi_name_, FileName(song_path).c_str(), _TRUNCATE);
    song_modified_ = false;
  }
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
  for (int st = Mixer::kKitFirst; st < Mixer::kAllStrips; ++st) {  // drum kit groups of channel 10
    IniFile::Section section{StripTarget(st), {}};
    WriteStripParams(section, mixer.strip(st), false);
    if (!section.values.empty()) ini.sections.push_back(section);
  }
  for (int t = 0; t < 64; ++t) {
    const bool mute = midi_synth_->track_muted(t), solo = midi_synth_->track_solo(t);
    if (!mute && !solo) continue;
    auto& section = ini.Add("track " + std::to_string(t + 1));
    if (mute) section.Set("mute", 1);
    if (solo) section.Set("solo", 1);
  }
  auto& master_section = ini.Add("master");
  WriteMasterParams(master_section, mixer.master());
  if (!ir_path_.empty()) master_section.Set("reverb.ir_file", ir_path_);
  auto& transport = ini.Add("transport");
  transport.Set("speed", midi_synth_->speed());
  transport.Set("metronome", midi_synth_->metronome() ? 1 : 0);
  transport.Set("count_in", count_in_ ? 1 : 0);
  transport.Set("loop", midi_synth_->loop_on() ? 1 : 0);
  transport.Set("loop_start_ms", midi_synth_->loop_start_ms());
  transport.Set("loop_end_ms", midi_synth_->loop_end_ms());
  // automation lanes (ticks of the session's song) and MIDI controller mappings
  const auto& lanes = midi_synth_->song().automation;
  for (size_t i = 0; i < lanes.size(); ++i) {
    auto& a = ini.Add("automation " + std::to_string(i + 1));
    a.Set("target", StripTarget(lanes[i].strip));
    a.Set("param", lanes[i].strip < 0 ? MasterParamKey(lanes[i].param) : StripParamKey(lanes[i].param));
    std::string points;
    char buf[64];
    for (auto& p : lanes[i].points) {
      snprintf(buf, sizeof(buf), "%s%llu:%.6g", points.empty() ? "" : " ", (unsigned long long)p.tick, p.value);
      points += buf;
    }
    a.Set("points", points);
  }
  {
    IniFile::Section map{"midi map", {}};
    for (int ch = 0; ch < 16; ++ch)
      for (int cc = 0; cc < 128; ++cc) {
        int strip, param;
        if (!midi_synth_->MappedTarget(ch, cc, &strip, &param)) continue;
        map.Set("ch" + std::to_string(ch + 1) + ".cc" + std::to_string(cc),
                StripTarget(strip) + "/" + (strip < 0 ? MasterParamKey(param) : StripParamKey(param)));
      }
    if (!map.values.empty()) ini.sections.push_back(map);
  }
  for (size_t i = 0; i < markers_.size(); ++i) {
    auto& m = ini.Add("marker " + std::to_string(i + 1));
    m.Set("ms", markers_[i].ms);
    m.Set("name", markers_[i].name);
  }
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
  for (int st = Mixer::kKitFirst; st < Mixer::kAllStrips; ++st)
    if (const auto* section = ini.Find(StripTarget(st))) ReadStripParams(*section, mixer.strip(st), false);
  ir_path_.clear();
  if (auto* master = ini.Find("master")) {
    ReadMasterParams(*master, mixer.master());
    const int ir = int(lroundf(mixer.master().get(kRevIr)));
    const std::string* file = master->Get("reverb.ir_file");
    if (ir == 3 && file && !file->empty()) {
      if (!LoadIrFile(*file)) mixer.master().set(kRevIr, 1);
    }
    if (int(lroundf(mixer.master().get(kRevIr))) < 3) mixer.SetConvolutionPreset(int(lroundf(mixer.master().get(kRevIr))));
  }
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
  selected_strip_ = std::clamp(int(s.GetNumber("selected_strip", 0)) - 1, -1, Mixer::kAllStrips - 1);
  if (Mixer::IsKitStrip(selected_strip_)) drum_strips_open_ = true;
  if (const auto* transport = ini.Find("transport")) {
    const double speed = std::clamp(transport->GetNumber("speed", 1.0), 0.5, 1.5);
    midi_synth_->set_speed(speed);
    speed_param_ = float(speed);
    midi_synth_->set_metronome(transport->GetNumber("metronome", 0) != 0);
    count_in_ = transport->GetNumber("count_in", 0) != 0;
    midi_synth_->set_loop(transport->GetNumber("loop", 0) != 0, transport->GetNumber("loop_start_ms", 0),
                          transport->GetNumber("loop_end_ms", 0));
  }
  // automation lanes go into the song; MIDI mappings into the synth
  if (midi_synth_->song().loaded) {
    dsp::audio::synth::SongInfo song = midi_synth_->song();
    song.automation.clear();
    for (int i = 1;; ++i) {
      const auto* a = ini.Find("automation " + std::to_string(i));
      if (!a) break;
      const std::string target = a->Get("target") ? *a->Get("target") : "";
      const std::string key = a->Get("param") ? *a->Get("param") : "";
      dsp::audio::synth::AutomationLane lane;
      lane.strip = ParseStripTarget(target);
      if (lane.strip < -1) continue;
      lane.param = lane.strip < 0 ? FindMasterParam(key.c_str()) : FindStripParam(key.c_str());
      if (lane.param < 0) continue;
      const std::string points = a->Get("points") ? *a->Get("points") : "";
      for (const char* p = points.c_str(); *p;) {
        char* end = nullptr;
        const unsigned long long tick = strtoull(p, &end, 10);
        if (end == p || *end != ':') break;
        p = end + 1;
        const float value = strtof(p, &end);
        if (end == p) break;
        lane.points.push_back({tick, value, 0});
        p = end;
        while (*p == ' ') ++p;
      }
      song.automation.push_back(lane);
    }
    if (!song.automation.empty()) InstallEditedSong(std::move(song), false);
  }
  for (int ch = 0; ch < 16; ++ch)
    for (int cc = 0; cc < 128; ++cc) midi_synth_->SetMidiMapping(ch, cc, 0, -1);
  if (const auto* map = ini.Find("midi map")) {
    for (auto& kv : map->values) {
      int ch = 0, cc = 0;
      if (sscanf_s(kv.first.c_str(), "ch%d.cc%d", &ch, &cc) != 2 || ch < 1 || ch > 16 || cc < 0 || cc > 127) continue;
      const size_t slash = kv.second.find('/');
      if (slash == std::string::npos) continue;
      const std::string target = kv.second.substr(0, slash), key = kv.second.substr(slash + 1);
      const int strip = ParseStripTarget(target);
      if (strip < -1) continue;
      const int param = strip < 0 ? FindMasterParam(key.c_str()) : FindStripParam(key.c_str());
      if (param >= 0) midi_synth_->SetMidiMapping(ch - 1, cc, strip, param);
    }
  }
  markers_.clear();
  for (int i = 1;; ++i) {
    const auto* m = ini.Find("marker " + std::to_string(i));
    if (!m) break;
    markers_.push_back({m->GetNumber("ms", 0), m->Get("name") ? *m->Get("name") : "M" + std::to_string(i)});
  }
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
  if (midi_synth_->song().TotalNotes() == 0) {
    ShowMessage("Nothing to export yet: open a MIDI file or record/draw some notes", true);
    return;
  }
  char path[MAX_PATH] = {};
  strncpy_s(path, midi_path_.empty() ? "Untitled.wav" : ReplaceExtension(midi_path_, ".wav").c_str(), _TRUNCATE);
  if (!PromptForFile(true, "WAV audio (*.wav)\0*.wav\0", "wav", "Export WAV (24-bit)", path, sizeof(path)))
    return;
  // The settings are copied now: changes made while it renders do not affect this export.
  auto settings = std::make_unique<ExportSettings>();
  settings->CaptureFrom(*midi_synth_);
  settings->midi_path = midi_path_;
  settings->wav_path = path;
  settings->ir_path = ir_path_;
  settings->sample_rate = synth_player_->output().sample_rate() ? synth_player_->output().sample_rate() : 48000;
  exporter_.Start(*settings);
}

bool ClientMainWindow::LoadIrFile(const std::string& path) {
  std::vector<float> l, r;
  std::string error;
  auto& mixer = midi_synth_->mixer;
  if (!dsp::audio::synth::effects::ReadImpulseResponse(path.c_str(), mixer.sample_rate(), 8.0, l, r, &error)) {
    ShowMessage("Could not load the impulse response: " + error, true);
    return false;
  }
  mixer.SetConvolutionIr(l, r, FileName(path));
  mixer.master().set(kRevIr, 3);
  mixer.master().set(kRevMode, 1);
  ir_path_ = path;
  char text[MAX_PATH + 64];
  snprintf(text, sizeof(text), "Loaded impulse response %s (%.1f s)", FileName(path).c_str(), l.size() / double(mixer.sample_rate()));
  ShowMessage(text);
  return true;
}

void ClientMainWindow::PromptLoadIr() {
  char path[MAX_PATH] = {};
  if (PromptForFile(false, "Impulse responses (*.wav)\0*.wav\0All files (*.*)\0*.*\0", nullptr,
                    "Load impulse response", path, sizeof(path)))
    LoadIrFile(path);
}

void ClientMainWindow::PromptExportMidi() {
  const auto& song = midi_synth_->song();
  if (!song.loaded || song.TotalNotes() == 0) {
    ShowMessage("Nothing to export yet: open a MIDI file or record/draw some notes", true);
    return;
  }
  char path[MAX_PATH] = {};
  strncpy_s(path, midi_path_.empty() ? "Untitled.mid" : ReplaceExtension(midi_path_, " (edited).mid").c_str(), _TRUNCATE);
  if (!PromptForFile(true, "MIDI files (*.mid)\0*.mid\0", "mid", "Export MIDI file", path, sizeof(path)))
    return;
  if (SaveSongAsMidi(song, path)) ShowMessage("Exported " + FileName(path));
  else ShowMessage(std::string("Could not write ") + path, true);
}

void ClientMainWindow::PollExport() {
  const auto state = exporter_.state();
  if (state == Exporter::kDone || state == Exporter::kFailed || state == Exporter::kCancelled) {
    ShowMessage(exporter_.result(), state == Exporter::kFailed);
    exporter_.Acknowledge();
  }
}

}
