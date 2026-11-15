// Piano roll: edit one track's notes. Double-click empty space adds a note, double-click a note
// deletes it; drag moves (snapped to the grid), drag the right edge resizes, drag on empty space
// selects with a rectangle; the velocity lane below sets velocities. Every edit is undoable.
#include "global.h"

namespace demo {

using namespace dsp::audio::synth;

namespace {

int FirstChannelOf(uint16_t mask) {
  for (int c = 0; c < 16; ++c)
    if (mask & (1 << c)) return c;
  return 0;
}

bool IsBlackKey(int note) {
  const int n = note % 12;
  return n == 1 || n == 3 || n == 6 || n == 8 || n == 10;
}

const char* kNoteNames[12] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};

}  // namespace

void ClientMainWindow::OpenEditor(int track) {
  const auto& song = midi_synth_->song();
  if (track < 0 || track >= int(song.tracks.size())) return;
  editor_track_ = track;
  const auto& t = song.tracks[track];
  if (!t.notes.empty()) {
    ed_top_note_ = float(std::min(127, t.high_note + 6));
    ed_view_start_ms_ = std::max(0.0, double(t.notes.front().start_ms) - 500.0);
  } else {
    ed_top_note_ = 84;
    ed_view_start_ms_ = std::max(0.0, PlayheadMs() - 1000.0);
  }
  // about eight bars across
  int beats_per_bar = 4;
  const double beat_ms = BeatMsAt(ed_view_start_ms_, &beats_per_bar);
  ed_ms_per_px_ = beat_ms * beats_per_bar * 8 / std::max(300.0f, lanes_w_ - 60);
  ed_last_length_ = song.ticks_per_beat ? song.ticks_per_beat / 2 : 240;
  ed_sel_gen_ = 0;  // resync
  ed_fit_pending_ = true;  // fit the pitch range once the lane height is known
  if (t.channel_mask) selected_strip_ = FirstChannelOf(t.channel_mask);
}

void ClientMainWindow::SyncEditorSelection() {
  const auto& song = midi_synth_->song();
  if (editor_track_ < 0 || editor_track_ >= int(song.tracks.size())) return;
  const auto& notes = song.tracks[editor_track_].notes;
  if (ed_sel_gen_ == song_gen_ && ed_sel_track_ == editor_track_ && ed_sel_.size() == notes.size()) return;
  ed_sel_.resize(notes.size());
  for (size_t i = 0; i < notes.size(); ++i) ed_sel_[i] = notes[i].selected;
  ed_sel_gen_ = song_gen_;
  ed_sel_track_ = editor_track_;
}

// Copy of the song with the current selection written into the edited track, handed to fn,
// then committed as one undoable edit.
void ClientMainWindow::EditTrack(const std::function<void(SongTrack&, SongInfo&)>& fn) {
  SongInfo song = midi_synth_->song();
  if (editor_track_ < 0 || editor_track_ >= int(song.tracks.size())) return;
  auto& track = song.tracks[editor_track_];
  for (size_t i = 0; i < track.notes.size(); ++i) track.notes[i].selected = i < ed_sel_.size() && ed_sel_[i];
  fn(track, song);
  CommitSong(std::move(song));
}

void ClientMainWindow::Audition(int note, int channel, int velocity) {
  if (recording_) return;  // would end up in the take
  if (ed_audition_note_ >= 0) SendLive(uint8_t(0x80 | ed_audition_channel_), uint8_t(ed_audition_note_), 64);
  ed_audition_note_ = -1;
  if (note < 0) return;
  SendLive(uint8_t(0x90 | (channel & 15)), uint8_t(note & 127), uint8_t(std::clamp(velocity, 1, 127)));
  ed_audition_note_ = note;
  ed_audition_channel_ = channel & 15;
}

void ClientMainWindow::DeleteSelection() {
  SyncEditorSelection();
  bool any = false;
  for (char s : ed_sel_) any |= s != 0;
  if (!any) return;
  EditTrack([](SongTrack& t, SongInfo&) {
    t.notes.erase(std::remove_if(t.notes.begin(), t.notes.end(), [](const SongNote& n) { return n.selected; }),
                  t.notes.end());
  });
}

void ClientMainWindow::QuantizeSelection() {
  SyncEditorSelection();
  const auto& song = midi_synth_->song();
  const uint64_t grid = song.ticks_per_beat * 4 / uint64_t(ed_snap_div_ ? ed_snap_div_ : 16);
  bool any = false;
  for (char s : ed_sel_) any |= s != 0;
  EditTrack([&](SongTrack& t, SongInfo&) {
    for (auto& n : t.notes) {
      if (any && !n.selected) continue;
      const uint64_t len = n.end_tick - n.start_tick;
      const uint64_t start = (n.start_tick + grid / 2) / grid * grid;
      if (start == n.start_tick) continue;
      n.start_tick = start;
      n.end_tick = start + len;
      n.MarkEdited();
    }
  });
  ShowMessage(any ? "Quantized the selected notes" : "Quantized every note in the track");
}

bool ClientMainWindow::EditorKey(WPARAM key, bool ctrl, bool shift) {
  if (editor_track_ < 0) return false;
  SyncEditorSelection();
  bool any = false;
  for (char s : ed_sel_) any |= s != 0;
  if (key == VK_DELETE || key == VK_BACK) {
    DeleteSelection();
    return true;
  }
  if (ctrl && key == 'A') {
    std::fill(ed_sel_.begin(), ed_sel_.end(), char(1));
    return true;
  }
  if (ctrl && key == 'Q') {
    QuantizeSelection();
    return true;
  }
  if (!any || ctrl) return false;
  const auto& song = midi_synth_->song();
  if (key == VK_UP || key == VK_DOWN) {  // transpose the selection (Shift = octave)
    const int d = (key == VK_UP ? 1 : -1) * (shift ? 12 : 1);
    EditTrack([d](SongTrack& t, SongInfo&) {
      for (auto& n : t.notes)
        if (n.selected) {
          n.note = uint8_t(std::clamp(int(n.note) + d, 0, 127));
          n.MarkEdited();
        }
    });
    return true;
  }
  if (key == VK_LEFT || key == VK_RIGHT) {  // nudge by one grid step
    const int64_t grid = int64_t(song.ticks_per_beat * 4 / uint64_t(ed_snap_div_ ? ed_snap_div_ : 16));
    const int64_t d = key == VK_RIGHT ? grid : -grid;
    EditTrack([d](SongTrack& t, SongInfo&) {
      for (auto& n : t.notes)
        if (n.selected) {
          const int64_t len = int64_t(n.end_tick - n.start_tick);
          const int64_t start = std::max<int64_t>(0, int64_t(n.start_tick) + d);
          n.start_tick = uint64_t(start);
          n.end_tick = uint64_t(start + len);
          n.MarkEdited();
        }
    });
    return true;
  }
  return false;
}

void ClientMainWindow::DrawPianoRoll(const Rect& area) {
  Gfx& g = gfx_;
  const SongInfo& song = midi_synth_->song();
  if (editor_track_ < 0 || editor_track_ >= int(song.tracks.size())) {
    editor_track_ = -1;
    return;
  }
  SyncEditorSelection();
  const SongTrack& track = song.tracks[editor_track_];
  const int ch = FirstChannelOf(track.channel_mask);
  const Color color = theme::Channel(ch);
  const uint64_t tpb = song.ticks_per_beat ? song.ticks_per_beat : 480;
  const uint64_t snap = ed_snap_div_ ? tpb * 4 / uint64_t(ed_snap_div_) : 1;

  // ---- header ----
  const Rect header(area.x, area.y, area.w, 32);
  g.Fill(header, theme::kPanel);
  float hx = header.x + 10;
  if (ui_.Button(Id(this, 90), Rect(hx, header.y + 5, 70, 22), "\xE2\x97\x80 Tracks", false, theme::kAccent, 11)) {
    Audition(-1, 0, 0);
    editor_track_ = -1;
    return;
  }
  hx += 78;
  g.Fill(Rect(hx, header.y + 8, 4, 16), color);
  char title[160];
  snprintf(title, sizeof(title), "%s   \xC2\xB7   Ch %d  %s", track.name.empty() ? "(untitled)" : track.name.c_str(), ch + 1,
           ProgramName(midi_synth_->channel_program(ch)));
  g.Text(title, Rect(hx + 10, header.y, 300, header.h), theme::kText, 12.5f, Align::kLeft, true);
  hx += 320;
  char snap_label[32];
  if (ed_snap_div_) snprintf(snap_label, sizeof(snap_label), "Snap 1/%d", ed_snap_div_);
  else snprintf(snap_label, sizeof(snap_label), "Snap off");
  if (ui_.Button(Id(this, 91), Rect(hx, header.y + 5, 76, 22), snap_label, ed_snap_div_ != 0, theme::kAccent, 11)) {
    static const int kDivs[] = {4, 8, 16, 32, 0};
    int i = 0;
    while (i < 5 && kDivs[i] != ed_snap_div_) ++i;
    ed_snap_div_ = kDivs[(i + 1) % 5];
  }
  hx += 80;
  if (ui_.Button(Id(this, 92), Rect(hx, header.y + 5, 72, 22), "Quantize", false, theme::kAccent, 11)) QuantizeSelection();
  if (ui_.Hover(Rect(hx, header.y + 5, 72, 22))) ui_.Tooltip("Snap note starts to the grid (selection, or all)  Ctrl+Q", hx + 36, header.y + 5);
  hx += 80;
  if (ui_.Button(Id(this, 93), Rect(hx, header.y + 5, 52, 22), "Undo", false, theme::kAccent, 11)) Undo();
  hx += 56;
  if (ui_.Button(Id(this, 94), Rect(hx, header.y + 5, 52, 22), "Redo", false, theme::kAccent, 11)) Redo();
  hx += 60;
  if (area.w > 1000)
    g.Text("dbl-click: add/delete  \xC2\xB7  drag: move  \xC2\xB7  edge: length  \xC2\xB7  Del  \xC2\xB7  \xE2\x86\x91\xE2\x86\x93 transpose",
           Rect(hx, header.y, header.r() - hx - 120, header.h), theme::kFaint, 10.5f);
  const Rect del_r(header.r() - 108, header.y + 5, 96, 22);
  if (ui_.Button(Id(this, 95), del_r, "Delete track", false, theme::kRed, 11)) {
    DeleteTrack(editor_track_);
    return;
  }

  // ---- layout ----
  const float key_w = 56, ruler_h = 34, hbar_h = 12;
  const float vel_h = std::clamp((area.h - 32) * 0.18f, 40.0f, 72.0f);
  const Rect body(area.x, header.b(), area.w, area.h - header.h);
  const Rect ruler(body.x + key_w, body.y, body.w - key_w, ruler_h);
  const Rect lanes(ruler.x, ruler.b(), ruler.w, body.h - ruler_h - vel_h - hbar_h);
  const Rect keys(body.x, lanes.y, key_w, lanes.h);
  const Rect vel(lanes.x, lanes.b(), lanes.w, vel_h);
  const Rect hbar(lanes.x, vel.b(), lanes.w, hbar_h);
  const double length = std::max({30000.0, song.length_ms + 8000.0});
  ed_ms_per_px_ = std::clamp(ed_ms_per_px_, 0.2, length / 100.0);
  if (ed_fit_pending_) {  // show the track's whole pitch range (plus a little), if it fits
    ed_fit_pending_ = false;
    const int lo = track.notes.empty() ? 48 : track.low_note, hi = track.notes.empty() ? 84 : track.high_note;
    const int range = std::max(12, hi - lo + 1 + 4);
    ed_key_h_ = std::clamp(lanes.h / range, 6.0f, 18.0f);
    ed_top_note_ = float(std::min(127, hi + 2 + std::max(0, int(lanes.h / ed_key_h_) - range) / 2));
  }
  ed_key_h_ = std::clamp(ed_key_h_, 5.0f, 28.0f);
  const float visible_notes = lanes.h / ed_key_h_;
  ed_top_note_ = std::clamp(ed_top_note_, visible_notes - 1, 127.0f);

  // wheel: notes up/down, Shift = time, Ctrl = zoom time, Ctrl+Shift = zoom keys
  const float wheel = ui_.TakeWheel(Rect(body.x, lanes.y, body.w, lanes.h + vel_h));
  if (wheel != 0) {
    if (ui_.in.ctrl && ui_.in.shift) {
      ed_key_h_ = std::clamp(ed_key_h_ * powf(1.15f, wheel), 5.0f, 28.0f);
    } else if (ui_.in.ctrl) {
      const double at = ed_view_start_ms_ + (ui_.in.mx - lanes.x) * ed_ms_per_px_;
      ed_ms_per_px_ = std::clamp(ed_ms_per_px_ * pow(0.8, wheel), 0.2, length / 100.0);
      ed_view_start_ms_ = at - (ui_.in.mx - lanes.x) * ed_ms_per_px_;
    } else if (ui_.in.shift) {
      ed_view_start_ms_ -= wheel * 90 * ed_ms_per_px_;
    } else {
      ed_top_note_ += wheel * 3;
    }
  }
  ed_top_note_ = std::clamp(ed_top_note_, visible_notes - 1, 127.0f);
  const double view_ms = lanes.w * ed_ms_per_px_;
  const bool playing = synth_player_->state() == RealtimePlayer::kStatePlaying;
  const double pos = PlayheadMs();
  if (follow_playhead_ && playing && ed_drag_ == 0 && ruler_drag_ == 0 &&
      (pos > ed_view_start_ms_ + view_ms * 0.92 || pos < ed_view_start_ms_))
    ed_view_start_ms_ = pos - view_ms * 0.08;
  ed_view_start_ms_ = std::clamp(ed_view_start_ms_, 0.0, std::max(0.0, length - view_ms * 0.5));

  auto x_ms = [&](double ms) { return lanes.x + float((ms - ed_view_start_ms_) / ed_ms_per_px_); };
  auto x_tick = [&](uint64_t t) { return x_ms(song.TickToMs(t)); };
  auto tick_at = [&](float px) { return song.MsToTick(std::max(0.0, ed_view_start_ms_ + (px - lanes.x) * ed_ms_per_px_)); };
  auto y_note = [&](int n) { return lanes.y + (ed_top_note_ - n) * ed_key_h_; };
  auto note_at = [&](float py) { return int(floorf(ed_top_note_ - (py - lanes.y) / ed_key_h_)) + 1; };
  auto snap_tick = [&](uint64_t t) { return snap > 1 ? (t + snap / 2) / snap * snap : t; };
  auto snap_down = [&](uint64_t t) { return snap > 1 ? t / snap * snap : t; };

  // ---- note rows and keyboard ----
  g.Fill(lanes, theme::kBg);
  g.PushClip(Rect(body.x, lanes.y, body.w, lanes.h));
  const int top = int(ed_top_note_) + 1, bottom = std::max(0, int(ed_top_note_ - visible_notes) - 1);
  for (int n = std::min(127, top); n >= bottom; --n) {
    const float y = y_note(n);
    if (IsBlackKey(n)) g.Fill(Rect(lanes.x, y, lanes.w, ed_key_h_), Rgb(0x0e1014));
    if (n % 12 == 0) g.Line(lanes.x, y + ed_key_h_ - 0.5f, lanes.r(), y + ed_key_h_ - 0.5f, theme::kLine, 1);
    const Rect key(keys.x, y, keys.w, ed_key_h_);
    g.Fill(key, IsBlackKey(n) ? Rgb(0x23262d) : Rgb(0xc9ccd3));
    g.Line(key.x, key.b() - 0.5f, key.r(), key.b() - 0.5f, Rgb(0x55595f), 1);
    if (n % 12 == 0 && ed_key_h_ >= 8) {
      char label[8];
      snprintf(label, sizeof(label), "%s%d", kNoteNames[n % 12], n / 12 - 1);
      g.Text(label, Rect(key.x, key.y, key.w - 4, ed_key_h_), Rgb(0x1a1d24), std::min(10.5f, ed_key_h_ - 1), Align::kRight, true);
    }
  }
  g.PopClip();
  // keyboard: press to hear a note
  if (ui_.Press(Id(this, 82), keys)) Audition(note_at(ui_.in.my), ch, 100);
  if (ui_.active == Id(this, 82)) {
    if (!ui_.in.down) {
      Audition(-1, 0, 0);
      ui_.active = 0;
    } else if (note_at(ui_.in.my) != ed_audition_note_) {
      Audition(note_at(ui_.in.my), ch, 100);
    }
  }

  // ---- grid: snap lines, beats, bars ----
  g.PushClip(lanes);
  const uint64_t view_t0 = tick_at(lanes.x), view_t1 = tick_at(lanes.r());
  if (snap > 1 && (x_tick(snap * 2) - x_tick(snap)) > 7) {
    for (uint64_t t = view_t0 / snap * snap; t <= view_t1; t += snap) {
      const float lx = x_tick(t);
      g.Line(lx, lanes.y, lx, lanes.b(), Rgb(0x181b21), 1);
    }
  }
  for (const auto& b : song.beats) {
    if (b.tick + tpb < view_t0) continue;
    if (b.tick > view_t1) break;
    const float lx = x_tick(b.tick);
    g.Line(lx, lanes.y, lx, lanes.b(), b.beat == 0 ? theme::kLine : Mix(theme::kLine, theme::kBg, 0.55f), 1);
  }

  // ---- notes (with a live preview while dragging) ----
  const auto& notes = track.notes;
  const bool dragging_notes = ed_drag_ == 1 || ed_drag_ == 2;
  auto preview = [&](size_t i, uint64_t& s, uint64_t& e, int& n) {
    s = notes[i].start_tick;
    e = notes[i].end_tick;
    n = notes[i].note;
    if (!dragging_notes || i >= ed_sel_.size() || !ed_sel_[i]) return;
    if (ed_drag_ == 1) {
      const int64_t ns = std::max<int64_t>(0, int64_t(s) + ed_drag_ticks_);
      e = uint64_t(ns + int64_t(e - s));
      s = uint64_t(ns);
      n = std::clamp(n + ed_drag_notes_, 0, 127);
    } else {
      e = uint64_t(std::max<int64_t>(int64_t(s) + int64_t(std::max<uint64_t>(snap, 1)), int64_t(e) + ed_drag_ticks_));
    }
  };
  int hit = -1;
  bool hit_edge = false;
  for (size_t i = 0; i < notes.size(); ++i) {
    uint64_t s, e;
    int n;
    preview(i, s, e, n);
    if (e < view_t0 || s > view_t1) continue;
    const float x0 = x_tick(s), x1 = std::max(x0 + 3.0f, x_tick(e));
    const Rect nr(x0, y_note(n) + 1, x1 - x0 - 1, ed_key_h_ - 2);
    const bool sel = i < ed_sel_.size() && ed_sel_[i];
    const bool sounding = playing && notes[i].start_ms <= pos && notes[i].end_ms > pos;
    Color c = Mix(color, Rgb(0x000000), 0.45f * (1.0f - notes[i].velocity / 127.0f));
    if (sounding) c = Mix(c, Rgb(0xffffff), 0.4f);
    g.FillRound(nr, 2, sel ? Mix(c, Rgb(0xffffff), 0.35f) : c);
    g.StrokeRound(nr, 2, sel ? Rgb(0xffffff) : Mix(color, Rgb(0x000000), 0.5f), sel ? 1.5f : 1.0f);
    if (ui_.Hover(nr) && ed_drag_ == 0) {
      hit = int(i);
      hit_edge = nr.w > 10 && ui_.in.mx > nr.r() - 6;
    }
  }
  // rubber band
  if (ed_drag_ == 3) {
    const Rect rb(std::min(ed_press_x_, ui_.in.mx), std::min(ed_press_y_, ui_.in.my), fabsf(ui_.in.mx - ed_press_x_),
                  fabsf(ui_.in.my - ed_press_y_));
    g.Fill(rb, WithAlpha(theme::kAccent, 0.12f));
    g.Stroke(rb, theme::kAccent, 1);
  }
  g.PopClip();
  if (hit >= 0 && hit_edge) SetCursor(LoadCursor(nullptr, IDC_SIZEWE));

  // ---- grid interaction ----
  const WidgetId grid_id = Id(this, 80);
  if (ui_.Hover(lanes) && ui_.in.pressed && ui_.active == 0 && ui_.in.dbl) {
    ui_.in.pressed = false;
    if (hit >= 0) {  // double-click a note: delete it
      const int index = hit;
      EditTrack([index](SongTrack& t, SongInfo&) { t.notes.erase(t.notes.begin() + index); });
    } else {         // double-click empty space: new note
      const int note = std::clamp(note_at(ui_.in.my), 0, 127);
      const uint64_t start = snap_down(tick_at(ui_.in.mx));
      const uint64_t len = std::max<uint64_t>(ed_last_length_, std::max<uint64_t>(snap, 1));
      std::fill(ed_sel_.begin(), ed_sel_.end(), char(0));
      EditTrack([&](SongTrack& t, SongInfo&) {
        SongNote n;
        n.start_tick = start;
        n.end_tick = start + len;
        n.note = uint8_t(note);
        n.velocity = 100;
        n.channel = uint8_t(ch);
        n.selected = true;
        n.MarkEdited();
        t.notes.push_back(n);
      });
      Audition(note, ch, 100);
      ed_audition_note_ = -1;  // a short blip: the note-off goes out right away
      SendLive(uint8_t(0x80 | ch), uint8_t(note), 64);
    }
  } else if (ui_.Press(grid_id, lanes)) {
    ed_press_x_ = ui_.in.mx;
    ed_press_y_ = ui_.in.my;
    ed_drag_ticks_ = 0;
    ed_drag_notes_ = 0;
    if (hit >= 0) {
      if (ui_.in.shift) ed_sel_[hit] = !ed_sel_[hit];
      else if (!ed_sel_[hit]) {
        std::fill(ed_sel_.begin(), ed_sel_.end(), char(0));
        ed_sel_[hit] = 1;
      }
      ed_grab_ = hit;
      ed_drag_ = hit_edge ? 2 : 1;
      Audition(notes[hit].note, notes[hit].channel, notes[hit].velocity);
    } else {
      if (!ui_.in.shift) std::fill(ed_sel_.begin(), ed_sel_.end(), char(0));
      ed_drag_ = 3;
    }
  }
  if (ui_.active == grid_id && ed_drag_ != 0) {
    if (ed_drag_ == 1 || ed_drag_ == 2) {
      int64_t dt = int64_t(tick_at(ui_.in.mx)) - int64_t(tick_at(ed_press_x_));
      if (snap > 1) dt = (dt >= 0 ? dt + int64_t(snap) / 2 : dt - int64_t(snap) / 2) / int64_t(snap) * int64_t(snap);
      ed_drag_ticks_ = dt;
      if (ed_drag_ == 1) {
        const int dn = int(lroundf((ed_press_y_ - ui_.in.my) / ed_key_h_));
        if (dn != ed_drag_notes_ && ed_grab_ >= 0 && ed_grab_ < int(notes.size()))
          Audition(std::clamp(notes[ed_grab_].note + dn, 0, 127), notes[ed_grab_].channel, notes[ed_grab_].velocity);
        ed_drag_notes_ = dn;
      }
    }
    if (!ui_.in.down) {  // release: apply
      const int mode = ed_drag_;
      ed_drag_ = 0;
      ui_.active = 0;
      Audition(-1, 0, 0);
      if (mode == 3) {
        const float x0 = std::min(ed_press_x_, ui_.in.mx), x1 = std::max(ed_press_x_, ui_.in.mx);
        const float y0 = std::min(ed_press_y_, ui_.in.my), y1 = std::max(ed_press_y_, ui_.in.my);
        if (x1 - x0 > 2 || y1 - y0 > 2)
          for (size_t i = 0; i < notes.size(); ++i) {
            const float nx0 = x_tick(notes[i].start_tick), nx1 = x_tick(notes[i].end_tick);
            const float ny0 = y_note(notes[i].note), ny1 = ny0 + ed_key_h_;
            if (nx1 >= x0 && nx0 <= x1 && ny1 >= y0 && ny0 <= y1) ed_sel_[i] = 1;
          }
      } else if (ed_drag_ticks_ != 0 || (mode == 1 && ed_drag_notes_ != 0)) {
        const int64_t dt = ed_drag_ticks_;
        const int dn = ed_drag_notes_;
        const uint64_t min_len = std::max<uint64_t>(snap, 1);
        uint64_t new_len = 0;
        EditTrack([&](SongTrack& t, SongInfo&) {
          for (auto& n : t.notes) {
            if (!n.selected) continue;
            if (mode == 1) {
              const int64_t start = std::max<int64_t>(0, int64_t(n.start_tick) + dt);
              n.end_tick = uint64_t(start + int64_t(n.end_tick - n.start_tick));
              n.start_tick = uint64_t(start);
              n.note = uint8_t(std::clamp(int(n.note) + dn, 0, 127));
            } else {
              n.end_tick = uint64_t(std::max<int64_t>(int64_t(n.start_tick + min_len), int64_t(n.end_tick) + dt));
              new_len = n.end_tick - n.start_tick;
            }
            n.MarkEdited();
          }
        });
        if (new_len) ed_last_length_ = new_len;  // new notes get the length last dragged
      }
    }
  }

  // ---- velocity lane ----
  g.Fill(vel, theme::kPanel);
  g.Line(vel.x, vel.y + 0.5f, vel.r(), vel.y + 0.5f, theme::kLine);
  g.Fill(Rect(body.x, vel.y, key_w, vel.h), theme::kPanel);
  g.Text("VEL", Rect(body.x, vel.y, key_w - 8, 18), theme::kDim, 10, Align::kRight, true);
  const WidgetId vel_id = Id(this, 81);
  int vel_hit = -1;
  g.PushClip(vel);
  for (size_t i = 0; i < notes.size(); ++i) {
    if (notes[i].end_tick < view_t0 || notes[i].start_tick > view_t1) continue;
    const float vx = x_tick(notes[i].start_tick);
    int v = notes[i].velocity;
    if (ed_drag_ == 4 && (int(i) == ed_grab_ || (ed_sel_[i] && ed_grab_ >= 0 && ed_sel_[ed_grab_])))
      v = std::clamp(int((vel.b() - 4 - ui_.in.my) / (vel.h - 8) * 127), 1, 127);
    const float vh = (vel.h - 8) * v / 127.0f;
    const bool sel = ed_sel_[i] != 0;
    g.Fill(Rect(vx, vel.b() - 4 - vh, 3, vh), sel ? Mix(color, Rgb(0xffffff), 0.35f) : color);
    g.Circle(vx + 1.5f, vel.b() - 4 - vh, 2.5f, sel ? Rgb(0xffffff) : color, true);
    if (fabsf(ui_.in.mx - vx - 1.5f) < 4 && ui_.Hover(vel)) vel_hit = int(i);
  }
  g.PopClip();
  if (vel_hit >= 0 && ui_.Press(vel_id, vel)) {
    ed_drag_ = 4;
    ed_grab_ = vel_hit;
  }
  if (ui_.active == vel_id && ed_drag_ == 4) {
    const int v = std::clamp(int((vel.b() - 4 - ui_.in.my) / (vel.h - 8) * 127), 1, 127);
    ui_.Tooltip(("Velocity " + std::to_string(v)).c_str(), ui_.in.mx, vel.y);
    if (!ui_.in.down) {
      ed_drag_ = 0;
      ui_.active = 0;
      const int grab = ed_grab_;
      const bool group = grab >= 0 && grab < int(ed_sel_.size()) && ed_sel_[grab];
      EditTrack([&](SongTrack& t, SongInfo&) {
        for (size_t i = 0; i < t.notes.size(); ++i)
          if (int(i) == grab || (group && t.notes[i].selected)) t.notes[i].velocity = uint8_t(v);
      });
    }
  }

  // ---- ruler, loop, playhead, scrollbar ----
  g.Fill(Rect(body.x, body.y, key_w, ruler_h), theme::kPanel);
  DrawRuler(ruler, ed_view_start_ms_, ed_ms_per_px_, lanes);
  DrawLoopAndPlayhead(ruler, Rect(lanes.x, lanes.y, lanes.w, lanes.h + vel_h), ed_view_start_ms_, ed_ms_per_px_);
  g.Fill(Rect(body.x, hbar.y, body.w, hbar.h), theme::kPanel);
  const float thumb_w = std::max(30.0f, float(hbar.w * std::min(1.0, view_ms / length)));
  const float thumb_x = hbar.x + float((hbar.w - thumb_w) * std::clamp(ed_view_start_ms_ / std::max(1.0, length - view_ms), 0.0, 1.0));
  const Rect thumb(thumb_x, hbar.y + 3, thumb_w, hbar.h - 6);
  const WidgetId bar_id = Id(this, 83);
  if (ui_.Press(bar_id, hbar)) hbar_grab_ = thumb.Contains(ui_.in.mx, ui_.in.my) ? ui_.in.mx - thumb_x : thumb_w * 0.5f;
  if (ui_.Dragging(bar_id)) {
    const float t = std::clamp((ui_.in.mx - hbar_grab_ - hbar.x) / std::max(1.0f, hbar.w - thumb_w), 0.0f, 1.0f);
    ed_view_start_ms_ = t * std::max(0.0, length - view_ms);
  }
  g.FillRound(thumb, 3, ui_.active == bar_id || ui_.Hover(hbar) ? theme::kFaint : theme::kPanel3);
  g.Line(lanes.x - 0.5f, ruler.y, lanes.x - 0.5f, vel.b(), theme::kLine);
}

}
