// Song editing (undo/redo, tracks), recording live input into new tracks, loop and markers.
#include "global.h"

namespace demo {

using namespace dsp::audio::synth;

// ---------------------------------------------------------------------------------------------
// Edits
// ---------------------------------------------------------------------------------------------
void ClientMainWindow::InstallEditedSong(SongInfo song) {
  // Building the playback events is the slow part; it happens here, outside the lock. The audio
  // thread is only kept out for the swap.
  auto prepared = MidiSynth::PrepareSong(std::move(song));
  const double pos = midi_synth_->PlaybackMs();
  synth_player_->WithSynthLocked([&] { midi_synth_->InstallSong(std::move(prepared), pos); });
  song_modified_ = true;
  ++song_gen_;
  RefreshTrackLists();
}

void ClientMainWindow::CommitSong(SongInfo song) {
  undo_.push_back(midi_synth_->song());
  if (undo_.size() > 100) undo_.erase(undo_.begin());
  redo_.clear();
  InstallEditedSong(std::move(song));
}

void ClientMainWindow::Undo() {
  if (undo_.empty()) return;
  redo_.push_back(midi_synth_->song());
  SongInfo song = std::move(undo_.back());
  undo_.pop_back();
  InstallEditedSong(std::move(song));
  ShowMessage("Undo");
}

void ClientMainWindow::Redo() {
  if (redo_.empty()) return;
  undo_.push_back(midi_synth_->song());
  SongInfo song = std::move(redo_.back());
  redo_.pop_back();
  InstallEditedSong(std::move(song));
  ShowMessage("Redo");
}

void ClientMainWindow::NewSong() {
  if (synth_player_ == nullptr || !synth_player_->output().running()) return;
  if (recording_) StopRecording();
  SongInfo song;
  MakeEmptySong(song);
  auto prepared = MidiSynth::PrepareSong(std::move(song));
  synth_player_->WithSynthLocked([&] { midi_synth_->InstallSong(std::move(prepared), -1.0); });
  midi_path_.clear();
  session_path_.clear();
  strncpy_s(midi_name_, "Untitled", _TRUNCATE);
  undo_.clear();
  redo_.clear();
  markers_.clear();
  midi_synth_->set_loop(false, 0, 0);
  song_modified_ = false;
  ++song_gen_;
  editor_track_ = -1;
  RefreshTrackLists();
  track_scroll_y_ = 0;
  view_start_ms_ = 0;
  ms_per_px_ = 30000.0 / std::max(200.0f, lanes_w_ - 24);  // 30 s across
  char title[MAX_PATH + 64];
  sprintf_s(title, "%s - Untitled", title_);
  SetWindowTextA(handle_, title);
}

void ClientMainWindow::RefreshTrackLists() {
  const auto& song = midi_synth_->song();
  visible_tracks_.clear();
  for (auto& n : strip_names_) n.clear();
  for (int t = 0; t < int(song.tracks.size()); ++t) {
    const auto& track = song.tracks[t];
    if (track.notes.empty() && !track.created) continue;
    visible_tracks_.push_back(t);
    for (int ch = 0; ch < 16; ++ch) {
      if (!(track.channel_mask & (1 << ch)) || !strip_names_[ch].empty()) continue;
      bool uses = track.notes.empty();  // a new empty track still names its channel
      for (auto& n : track.notes) if (n.channel == ch) { uses = true; break; }
      if (uses) strip_names_[ch] = track.name.empty() ? std::string("Track ") + std::to_string(t + 1) : track.name;
    }
  }
  if (editor_track_ >= int(song.tracks.size())) editor_track_ = -1;
}

// New empty track on the first unused channel (not 10), starting with a piano.
int ClientMainWindow::AddTrack() {
  SongInfo song = midi_synth_->song();
  if (!song.loaded) MakeEmptySong(song);
  uint16_t used = 0;
  for (auto& t : song.tracks) used |= t.channel_mask;
  int ch = 0;
  while (ch < 16 && (ch == 9 || (used & (1 << ch)))) ++ch;
  if (ch >= 16) ch = 0;
  SongTrack track;
  track.created = true;
  track.name = "Track " + std::to_string(song.tracks.size() + 1);
  SongEvent program;
  program.order = song.next_order++;
  program.ev.type = dsp::audio::midi::kEventTypeChannel;
  program.ev.subtype = dsp::audio::midi::kEventSubtypeProgramChange;
  program.ev.channel = uint8_t(ch);
  program.ev.data.program.number = 0;
  track.events.push_back(program);
  song.tracks.push_back(track);
  const int index = int(song.tracks.size()) - 1;
  CommitSong(std::move(song));
  return index;
}

void ClientMainWindow::DeleteTrack(int index) {
  SongInfo song = midi_synth_->song();
  if (index < 0 || index >= int(song.tracks.size())) return;
  const std::string name = song.tracks[index].name;
  // Track mute/solo are by index: move the ones above down by one.
  bool mute[64], solo[64];
  for (int t = 0; t < 64; ++t) {
    mute[t] = midi_synth_->track_muted(t);
    solo[t] = midi_synth_->track_solo(t);
  }
  song.tracks.erase(song.tracks.begin() + index);
  CommitSong(std::move(song));
  for (int t = 0; t < 64; ++t) {
    const int from = t < index ? t : t + 1;
    midi_synth_->set_track_muted(t, from < 64 && mute[from]);
    midi_synth_->set_track_solo(t, from < 64 && solo[from]);
  }
  if (editor_track_ == index) editor_track_ = -1;
  else if (editor_track_ > index) --editor_track_;
  ShowMessage("Deleted track " + name + "  (Ctrl+Z to undo)");
}

// ---------------------------------------------------------------------------------------------
// Recording: live input (MIDI keyboard or PC keys) becomes a new track when recording stops.
// ---------------------------------------------------------------------------------------------
void ClientMainWindow::StartRecording() {
  if (recording_ || synth_player_ == nullptr || !synth_player_->output().running()) return;
  if (!midi_synth_->song().loaded) NewSong();
  take_notes_.clear();
  take_events_.clear();
  for (auto& row : take_open_) for (auto& v : row) v = -1.0;
  RealtimePlayer::RecordedMessage stale;
  while (synth_player_->PopRecorded(stale)) {}  // nothing left over from an earlier take
  recording_ = true;
  midi_synth_->set_hold_at_end(true);  // keep going past the song's end
  synth_player_->set_recording(true);
  const bool playing = synth_player_->state() == RealtimePlayer::kStatePlaying;
  if (count_in_ && !playing) {
    int beats_per_bar = 4;
    const double beat_ms = BeatMsAt(PlayheadMs(), &beats_per_bar) / midi_synth_->speed();
    midi_synth_->StartCountIn(beat_ms * beats_per_bar, beat_ms);
  }
  synth_player_->Play();
}

void ClientMainWindow::PollRecording() {
  if (synth_player_ == nullptr) return;
  RealtimePlayer::RecordedMessage m;
  while (synth_player_->PopRecorded(m)) {
    if (!recording_) continue;
    const uint8_t status = uint8_t(m.message & 0xFF), type = status & 0xF0, ch = status & 0x0F;
    const uint8_t d1 = uint8_t((m.message >> 8) & 0x7F), d2 = uint8_t((m.message >> 16) & 0x7F);
    if (type == 0x90 && d2 > 0) {
      if (take_open_[ch][d1] >= 0)  // re-struck: close the held one first
        take_notes_.push_back({take_open_[ch][d1], m.song_ms, d1, take_velocity_[ch][d1], ch});
      take_open_[ch][d1] = m.song_ms;
      take_velocity_[ch][d1] = d2;
    } else if (type == 0x80 || type == 0x90) {
      if (take_open_[ch][d1] >= 0) {
        take_notes_.push_back({take_open_[ch][d1], m.song_ms, d1, take_velocity_[ch][d1], ch});
        take_open_[ch][d1] = -1.0;
      }
    } else if (type == 0xB0 || type == 0xE0) {
      take_events_.push_back({m.song_ms, m.message});
    }
  }
}

void ClientMainWindow::StopRecording() {
  if (!recording_) return;
  PollRecording();
  const double end_ms = PlayheadMs();
  for (int ch = 0; ch < 16; ++ch)
    for (int n = 0; n < 128; ++n)
      if (take_open_[ch][n] >= 0) {
        take_notes_.push_back({take_open_[ch][n], std::max(end_ms, take_open_[ch][n] + 1), uint8_t(n),
                               take_velocity_[ch][n], uint8_t(ch)});
        take_open_[ch][n] = -1.0;
      }
  recording_ = false;
  synth_player_->set_recording(false);
  midi_synth_->set_hold_at_end(false);
  if (take_notes_.empty()) {
    ShowMessage("Nothing recorded (play a MIDI keyboard or the PC keys while recording)", true);
    return;
  }
  SongInfo song = midi_synth_->song();
  SongTrack track;
  track.created = true;
  track.name = "Take " + std::to_string(++take_count_);
  for (auto& t : take_notes_) {
    SongNote n;
    n.start_tick = song.MsToTick(std::max(0.0, t.start_ms));
    n.end_tick = std::max(n.start_tick + 1, song.MsToTick(t.end_ms));
    n.note = t.note;
    n.velocity = t.velocity;
    n.channel = t.channel;
    n.MarkEdited();
    track.notes.push_back(n);
  }
  for (auto& e : take_events_) {
    SongEvent se;
    se.tick = song.MsToTick(std::max(0.0, e.ms));
    se.order = song.next_order++;
    const uint8_t status = uint8_t(e.message & 0xFF);
    se.ev.type = dsp::audio::midi::kEventTypeChannel;
    se.ev.channel = status & 0x0F;
    if ((status & 0xF0) == 0xB0) {
      se.ev.subtype = dsp::audio::midi::kEventSubtypeController;
      se.ev.data.controller.type = uint8_t((e.message >> 8) & 0x7F);
      se.ev.data.controller.value = uint8_t((e.message >> 16) & 0x7F);
    } else {
      se.ev.subtype = dsp::audio::midi::kEventSubtypePitchBend;
      se.ev.data.pitch.value = uint16_t(((e.message >> 8) & 0x7F) | (((e.message >> 16) & 0x7F) << 7));
    }
    track.events.push_back(se);
  }
  const size_t count = track.notes.size();
  song.tracks.push_back(std::move(track));
  CommitSong(std::move(song));
  char text[96];
  snprintf(text, sizeof(text), "Recorded Take %d: %zu notes  (Ctrl+Z to undo)", take_count_, count);
  ShowMessage(text);
}

// ---------------------------------------------------------------------------------------------
// Grid helpers, loop and markers
// ---------------------------------------------------------------------------------------------
double ClientMainWindow::BeatMsAt(double ms, int* beats_per_bar) const {
  const auto& beats = midi_synth_->song().beats;
  if (beats_per_bar) *beats_per_bar = 4;
  if (beats.size() < 2) return 500.0;
  auto it = std::upper_bound(beats.begin(), beats.end(), ms, [](double v, const SongBeat& b) { return v < b.ms; });
  size_t i = it == beats.begin() ? 0 : size_t(it - beats.begin()) - 1;
  if (i + 1 >= beats.size()) i = beats.size() - 2;
  if (beats_per_bar) {
    size_t start = i;
    while (start > 0 && beats[start].beat != 0) --start;
    int count = 1;
    while (start + count < beats.size() && beats[start + count].beat != 0) ++count;
    *beats_per_bar = count;
  }
  return beats[i + 1].ms - beats[i].ms;
}

double ClientMainWindow::SnapToBeat(double ms) const {
  const auto& beats = midi_synth_->song().beats;
  if (beats.empty()) return ms;
  auto it = std::lower_bound(beats.begin(), beats.end(), ms, [](const SongBeat& b, double v) { return b.ms < v; });
  double best = it == beats.end() ? beats.back().ms : it->ms;
  if (it != beats.begin() && fabs((it - 1)->ms - ms) < fabs(best - ms)) best = (it - 1)->ms;
  return best;
}

void ClientMainWindow::ToggleLoop() {
  double start = midi_synth_->loop_start_ms(), end = midi_synth_->loop_end_ms();
  if (end - start < 20.0) {  // no region yet: the bar under the playhead and the next three
    const auto& beats = midi_synth_->song().beats;
    const double pos = PlayheadMs();
    start = 0;
    int bar = 1;
    for (auto& b : beats) {
      if (b.ms > pos) break;
      if (b.beat == 0) { start = b.ms; bar = b.bar; }
    }
    end = start + 8000;
    for (auto& b : beats)
      if (b.beat == 0 && b.bar == bar + 4) { end = b.ms; break; }
  }
  midi_synth_->set_loop(!midi_synth_->loop_on(), start, end);
}

void ClientMainWindow::AddMarker() {
  const double ms = PlayheadMs();
  Marker m{ms, "M" + std::to_string(markers_.size() + 1)};
  markers_.insert(std::upper_bound(markers_.begin(), markers_.end(), m,
                                   [](const Marker& a, const Marker& b) { return a.ms < b.ms; }),
                  m);
  ShowMessage("Added marker " + m.name + "  (click to jump, drag to move, right-click to delete)");
}

}
