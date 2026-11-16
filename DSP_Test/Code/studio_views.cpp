// The studio panels: transport bar, track view, mixer and inspector. Immediate-mode: each
// function draws its panel and handles the mouse for it, every frame.
#include "global.h"

namespace demo {

using namespace dsp::audio::synth::effects;
using dsp::audio::synth::SongInfo;
using dsp::audio::synth::SongTrack;

namespace {

void FormatTime(double ms, char* out, size_t n, bool tenths) {
  if (ms < 0) ms = 0;
  const int total = int(ms / 100.0);  // tenths of a second
  if (tenths) snprintf(out, n, "%d:%02d.%d", total / 600, (total / 10) % 60, total % 10);
  else snprintf(out, n, "%d:%02d", total / 600, (total / 10) % 60);
}

int FirstChannel(uint16_t mask) {
  for (int c = 0; c < 16; ++c)
    if (mask & (1 << c)) return c;
  return 0;
}

const Color kBandColor[kEqBands] = {Rgb(0xffb84a), Rgb(0x3ddc84), Rgb(0x4ea1ff), Rgb(0xf279d8)};

}  // namespace

// ---------------------------------------------------------------------------------------------
// Transport bar
// ---------------------------------------------------------------------------------------------
void ClientMainWindow::DrawTopBar(const Rect& r) {
  Gfx& g = gfx_;
  g.Fill(r, theme::kPanel);
  g.Line(r.x, r.b() - 0.5f, r.r(), r.b() - 0.5f, theme::kLine);
  const float cy = r.cy();
  float x = r.x + 16;

  if (r.w >= 1540) {  // the name gives way to the buttons on narrower windows
    g.Text("DSP", Rect(x, cy - 11, 40, 22), theme::kAccent, 17, Align::kLeft, true);
    g.Text("Studio", Rect(x + 36, cy - 11, 70, 22), theme::kText, 17, Align::kLeft, false);
    x += 112;
  }

  auto button = [&](int salt, float w, const char* label, const char* tip, bool on = false, Color color = theme::kAccent) {
    const Rect br(x, cy - 15, w, 30);
    const bool clicked = ui_.Button(Id(this, salt), br, label, on, color);
    if (tip && ui_.Hover(br)) ui_.Tooltip(tip, br.cx(), br.y);
    x += w + 4;
    return clicked;
  };
  if (button(9, 50, "New", "New empty song (Ctrl+N)")) NewSong();
  if (button(1, 56, "Open", "Open a MIDI file or session (Ctrl+O)")) PromptAndOpen();
  if (button(6, 54, "Save", session_path_.empty() ? "Save session (Ctrl+S)" : "Save session (Ctrl+S, Shift = save as)"))
    PromptSaveSession(ui_.in.shift);
  const Rect export_r(x, cy - 15, 74, 30);
  if (button(7, 74, "Export \xE2\x96\xBE", "Export the song as audio (Ctrl+E) or as a MIDI file", export_menu_open_) &&
      !exporter_.busy()) {
    export_menu_open_ = !export_menu_open_;
    export_menu_anchor_ = Rect(export_r.x, export_r.b() + 4, 250, 0);
  }
  x += 14;

  // Transport: rewind, play/pause, stop, record
  const bool playing = synth_player_->state() == dsp::audio::synth::RealtimePlayer::kStatePlaying;
  const Color icon = theme::kText;
  Rect b(x, cy - 15, 36, 30);
  if (ui_.Button(Id(this, 2), b, "")) Seek(midi_synth_->loop_on() ? midi_synth_->loop_start_ms() : 0);
  g.Fill(Rect(b.cx() - 7, cy - 6, 2, 12), icon);
  g.FillTriangle(b.cx() + 6, cy - 6, b.cx() + 6, cy + 6, b.cx() - 4, cy, icon);
  b.x += 40;
  b.w = 44;
  if (ui_.Button(Id(this, 3), b, "", playing, theme::kGreen)) synth_player_->TogglePause();
  const Color play_icon = playing ? Rgb(0x0d0f13) : icon;
  if (playing) {
    g.Fill(Rect(b.cx() - 6, cy - 7, 4, 14), play_icon);
    g.Fill(Rect(b.cx() + 2, cy - 7, 4, 14), play_icon);
  } else {
    g.FillTriangle(b.cx() - 5, cy - 8, b.cx() - 5, cy + 8, b.cx() + 8, cy, play_icon);
  }
  b.x += 48;
  b.w = 36;
  if (ui_.Button(Id(this, 4), b, "")) {
    if (recording_) StopRecording();
    synth_player_->Stop();
  }
  g.Fill(Rect(b.cx() - 6, cy - 6, 12, 12), icon);
  b.x += 40;
  if (ui_.Button(Id(this, 10), b, "", recording_, theme::kRed)) {
    if (recording_) StopRecording();
    else StartRecording();
  }
  g.Circle(b.cx(), cy, 6, recording_ ? Rgb(0xffffff) : theme::kRed, true);
  if (ui_.Hover(b))
    ui_.Tooltip(recording_ ? "Stop recording: the take becomes a new track (Ctrl+R)"
                           : "Record live input from a MIDI keyboard or the PC keys (Ctrl+R)",
                b.cx(), b.y);
  x = b.r() + 16;

  // Position
  const auto& song = midi_synth_->song();
  const double pos = PlayheadMs();
  char now[32], len[32], line[96];
  FormatTime(pos, now, sizeof(now), true);
  FormatTime(song.length_ms, len, sizeof(len), false);
  g.FillRound(Rect(x, cy - 20, 226, 40), 5, Rgb(0x0e1014));
  g.Text(now, Rect(x + 12, cy - 19, 110, 26), recording_ ? theme::kRed : theme::kText, 21, Align::kLeft, false, true);
  snprintf(line, sizeof(line), "/ %s", len);
  g.Text(line, Rect(x + 112, cy - 17, 110, 22), theme::kDim, 13, Align::kLeft, false, true);
  int bar = 1, beat = 1;
  double bpm = 120;
  if (!song.beats.empty()) {
    auto it = std::upper_bound(song.beats.begin(), song.beats.end(), pos,
                               [](double v, const dsp::audio::synth::SongBeat& sb) { return v < sb.ms; });
    if (it != song.beats.begin()) {
      --it;
      bar = it->bar;
      beat = it->beat + 1;
    }
  }
  for (auto& t : song.tempo)
    if (t.ms <= pos) bpm = t.bpm;
  if (midi_synth_->counting_in()) {
    g.Text("COUNT-IN", Rect(x + 13, cy + 4, 210, 14), theme::kOrange, 10.5f, Align::kLeft, true, true);
  } else {
    const double speed = midi_synth_->speed();
    if (fabs(speed - 1.0) > 1e-3) snprintf(line, sizeof(line), "BAR %d.%d   %.1f BPM x%.2f", bar, beat, bpm, speed);
    else snprintf(line, sizeof(line), "BAR %d.%d    %.1f BPM", bar, beat, bpm);
    g.Text(line, Rect(x + 13, cy + 4, 210, 14), recording_ ? theme::kRed : theme::kDim, 10.5f, Align::kLeft, false, true);
  }
  x += 240;

  // File name / export progress
  const float right_block = 290;
  const Rect info(x, cy - 18, std::max(60.0f, r.r() - right_block - x), 36);
  if (exporter_.busy()) {
    const bool preparing = exporter_.state() == Exporter::kPreparing;
    char text[64];
    if (preparing) snprintf(text, sizeof(text), "Export: preparing instruments...");
    else snprintf(text, sizeof(text), "Exporting WAV  %.0f%%", exporter_.progress() * 100);
    g.Text(text, Rect(info.x, info.y, info.w - 70, 18), theme::kText, 12);
    const Rect bar_r(info.x, info.y + 23, info.w - 76, 6);
    g.FillRound(bar_r, 3, Rgb(0x0b0c10));
    g.FillRound(Rect(bar_r.x, bar_r.y, bar_r.w * exporter_.progress(), bar_r.h), 3, theme::kAccent);
    if (ui_.Button(Id(this, 8), Rect(info.r() - 66, cy - 12, 62, 24), "Cancel", false, theme::kAccent, 11))
      exporter_.Cancel();
  } else {
    std::string name = midi_name_[0] ? midi_name_ : "No file loaded";
    if (song_modified_) name += "  (edited)";
    g.Text(name.c_str(), Rect(info.x, session_path_.empty() ? cy - 10 : cy - 17, info.w, 20),
           midi_name_[0] ? theme::kText : theme::kFaint, 13);
    if (!session_path_.empty()) {
      const size_t slash = session_path_.find_last_of("\\/");
      const std::string session = "session: " + (slash == std::string::npos ? session_path_ : session_path_.substr(slash + 1));
      g.Text(session.c_str(), Rect(info.x, cy + 2, info.w, 16), theme::kDim, 10.5f);
    }
  }

  // Output meter (after the limiter) and limiter activity
  float rx = r.r() - right_block + 8;
  g.Text("OUT", Rect(rx, cy - 8, 30, 16), theme::kDim, 10.5f, Align::kLeft, true);
  const Rect meter(rx + 32, cy - 5, 110, 10);
  g.FillRound(meter, 2, Rgb(0x0b0c10));
  const float t = std::clamp((output_db_ + 60.0f) / 60.0f, 0.0f, 1.0f);
  const Color mc = output_db_ > -1.5f ? theme::kRed : output_db_ > -9 ? theme::kYellow : theme::kGreen;
  g.FillRound(Rect(meter.x, meter.y, meter.w * t, meter.h), 2, mc);
  char text[48];
  if (output_db_ <= -89) snprintf(text, sizeof(text), "-inf");
  else snprintf(text, sizeof(text), "%.1f", output_db_);
  g.Text(text, Rect(meter.r() + 6, cy - 8, 44, 16), theme::kText, 11, Align::kLeft, false, true);
  snprintf(text, sizeof(text), "LIM %.1f", limiter_db_);
  g.Text(text, Rect(meter.r() + 48, cy - 8, 70, 16), limiter_db_ < -0.5f ? theme::kOrange : theme::kFaint, 11,
         Align::kLeft, false, true);
}

// Export menu (popup under the Export button).
void ClientMainWindow::DrawExportMenu(const Rect& a) {
  Gfx& g = gfx_;
  const Rect box(a.x, a.y, a.w, 2 * 34 + 12);
  if (ui_.in.pressed && !box.Contains(ui_.in.mx, ui_.in.my)) {
    ui_.in.pressed = false;
    export_menu_open_ = false;
    return;
  }
  g.FillRound(Rect(box.x + 3, box.y + 5, box.w, box.h), 6, Rgb(0x000000, 0.45f));
  g.FillRound(box, 6, theme::kPanel2);
  g.StrokeRound(box, 6, theme::kLine);
  const char* labels[2] = {"Audio (WAV, 24-bit)...", "MIDI file..."};
  const char* keys[2] = {"Ctrl+E", ""};
  for (int i = 0; i < 2; ++i) {
    const Rect item(box.x + 6, box.y + 6 + i * 34, box.w - 12, 32);
    const bool over = ui_.Hover(item);
    if (over) g.FillRound(item, 4, theme::kPanel3);
    g.Text(labels[i], item.Inset(12, 0), theme::kText, 12.5f);
    g.Text(keys[i], Rect(item.x, item.y, item.w - 12, item.h), theme::kDim, 11, Align::kRight);
    if (ui_.Clicked(Id(this, 70 + i), item)) {
      export_menu_open_ = false;
      if (i == 0) PromptExport();
      else PromptExportMidi();
    }
  }
}

// ---------------------------------------------------------------------------------------------
// Status bar
// ---------------------------------------------------------------------------------------------
void ClientMainWindow::DrawStatusBar(const Rect& r) {
  Gfx& g = gfx_;
  g.Fill(r, theme::kPanel);
  g.Line(r.x, r.y + 0.5f, r.r(), r.y + 0.5f, theme::kLine);
  static const char* kNotes[] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
  char left[256], right[256];
  snprintf(left, sizeof(left),
           "PC keys  Z = %s%d   program %d %s (ch 1)      Space play/pause   Esc stop   Home rewind   "
           "\xE2\x86\x91\xE2\x86\x93 octave   \xE2\x86\x90\xE2\x86\x92 program   Shift sustain",
           kNotes[octave_base_ % 12], octave_base_ / 12 - 1, program_ + 1, ProgramName(program_));
  if (!message_.empty()) {  // save/export results take the place of the key help for a few seconds
    g.Fill(Rect(r.x, r.y + 1, r.w * 0.62f, r.h - 1), WithAlpha(message_error_ ? theme::kOrange : theme::kGreen, 0.12f));
    g.Text(message_.c_str(), Rect(r.x + 14, r.y, r.w * 0.62f - 20, r.h), message_error_ ? theme::kOrange : theme::kGreen,
           11.5f, Align::kLeft, true);
  } else {
    g.Text(left, Rect(r.x + 14, r.y, r.w * 0.62f, r.h), theme::kDim, 11);
  }

  int n = 0;
  if (synth_player_->output().running()) {
    auto& out = synth_player_->output();
    n = snprintf(right, sizeof(right), "WASAPI %u Hz  %.1f ms     ", out.sample_rate(), out.period_ms());
  } else {
    n = snprintf(right, sizeof(right), "%s     ", synth_player_->output().device_lost() ? "Audio device lost" : "No audio device");
  }
  if (midi_input_.open_count() == 0)
    snprintf(right + n, sizeof(right) - n, "MIDI in: none (F5 rescan)");
  else
    snprintf(right + n, sizeof(right) - n, "MIDI in: %s%s", midi_input_.port_name(0),
             midi_input_.open_count() > 1 ? " +more" : "");
  g.Text(right, Rect(r.x + r.w * 0.62f, r.y, r.w * 0.38f - 14, r.h), theme::kDim, 11, Align::kRight);
}

// ---------------------------------------------------------------------------------------------
// Track view
// ---------------------------------------------------------------------------------------------
// Ruler shared by the track view and the piano roll: markers on top (click = jump, drag = move,
// right-click = delete), bar numbers below. Click in the bar lane seeks; drag there sets the
// loop region (snapped to beats, Shift = free); drag the loop's edges to change it.
void ClientMainWindow::DrawRuler(const Rect& ruler, double view_start_ms, double ms_per_px, const Rect& lanes) {
  Gfx& g = gfx_;
  const SongInfo& song = midi_synth_->song();
  const float marker_h = 13;
  const Rect marker_lane(ruler.x, ruler.y, ruler.w, marker_h);
  const Rect bar_lane(ruler.x, ruler.y + marker_h, ruler.w, ruler.h - marker_h);
  auto x_of = [&](double ms) { return ruler.x + float((ms - view_start_ms) / ms_per_px); };
  auto ms_at = [&](float px) { return std::max(0.0, view_start_ms + (px - ruler.x) * ms_per_px); };
  const double view_end = view_start_ms + ruler.w * ms_per_px;

  g.Fill(ruler, theme::kPanel2);
  g.Fill(marker_lane, Mix(theme::kPanel2, theme::kBg, 0.35f));
  g.PushClip(ruler);

  // loop region
  const double loop_a = midi_synth_->loop_start_ms(), loop_b = midi_synth_->loop_end_ms();
  const bool loop_on = midi_synth_->loop_on();
  if (loop_b - loop_a >= 20) {
    const float la = x_of(loop_a), lb = x_of(loop_b);
    const Color lc = loop_on ? theme::kAccent : theme::kFaint;
    g.Fill(Rect(la, bar_lane.y, lb - la, bar_lane.h), WithAlpha(lc, loop_on ? 0.30f : 0.15f));
    g.Fill(Rect(la, bar_lane.y, 2, bar_lane.h), lc);
    g.Fill(Rect(lb - 2, bar_lane.y, 2, bar_lane.h), lc);
  }

  // bar numbers and beat ticks
  if (song.beats.size() > 1) {
    double bar_ms = 2000;
    for (size_t i = 1; i < song.beats.size(); ++i)
      if (song.beats[i].beat == 0) { bar_ms = song.beats[i].ms - song.beats[0].ms; break; }
    const float bar_px = float(bar_ms / ms_per_px);
    int bar_step = 1;
    while (bar_px * bar_step < 46 && bar_step < 4096) bar_step *= 2;
    const bool show_beats = bar_px / 4 > 9;
    for (const auto& b : song.beats) {
      if (b.ms < view_start_ms - bar_ms) continue;
      if (b.ms > view_end) break;
      const float bx = x_of(b.ms);
      if (b.beat == 0 && (b.bar - 1) % bar_step == 0) {
        char num[16];
        snprintf(num, sizeof(num), "%d", b.bar);
        g.Line(bx, bar_lane.y + 8, bx, ruler.b(), theme::kFaint, 1);
        g.Text(num, Rect(bx + 4, bar_lane.y + 1, 50, 14), theme::kDim, 10.5f);
      } else if (show_beats) {
        g.Line(bx, ruler.b() - 4, bx, ruler.b(), theme::kFaint, 1);
      }
    }
  }

  // markers
  int hover_marker = -1;
  for (int i = 0; i < int(markers_.size()); ++i) {
    const float mx = x_of(markers_[i].ms);
    const float w = g.TextWidth(markers_[i].name.c_str(), 9.5f, true) + 10;
    const Rect flag(mx, marker_lane.y + 1, w, marker_h - 2);
    if (flag.r() < ruler.x || flag.x > ruler.r()) continue;
    const bool over = ui_.Hover(flag);
    if (over) hover_marker = i;
    g.FillRound(flag, 2, over || marker_drag_ == i ? theme::kYellow : Mix(theme::kYellow, theme::kPanel2, 0.35f));
    g.Text(markers_[i].name.c_str(), flag.Inset(5, 0), Rgb(0x0d0f13), 9.5f, Align::kLeft, true);
  }
  g.PopClip();

  // --- interaction ---
  const WidgetId marker_id = Id(this, 32), bar_id = Id(this, 33);
  if (hover_marker >= 0 && ui_.in.rpressed) {  // right-click deletes
    markers_.erase(markers_.begin() + hover_marker);
    ui_.in.rpressed = false;
    hover_marker = -1;
  }
  if (hover_marker >= 0 && ui_.Press(marker_id, Rect(x_of(markers_[hover_marker].ms) - 2, marker_lane.y, 200, marker_h))) {
    marker_drag_ = hover_marker;
    ruler_drag_ = 5;
    ruler_press_x_ = ui_.in.mx;
  }
  const float edge = 5;
  const float la = x_of(loop_a), lb = x_of(loop_b);
  const bool has_loop = loop_b - loop_a >= 20;
  if (ui_.Press(bar_id, bar_lane)) {
    ruler_press_x_ = ui_.in.mx;
    if (has_loop && fabsf(ui_.in.mx - la) <= edge) ruler_drag_ = 3;
    else if (has_loop && fabsf(ui_.in.mx - lb) <= edge) ruler_drag_ = 4;
    else {
      ruler_drag_ = 1;
      ruler_anchor_ms_ = ms_at(ui_.in.mx);
    }
  }
  if (ui_.Hover(bar_lane) && ruler_drag_ == 0 && has_loop &&
      (fabsf(ui_.in.mx - la) <= edge || fabsf(ui_.in.mx - lb) <= edge))
    SetCursor(LoadCursor(nullptr, IDC_SIZEWE));
  auto snap = [&](double ms) { return ui_.in.shift ? ms : SnapToBeat(ms); };
  if (ruler_drag_ != 0 && (ui_.active == marker_id || ui_.active == bar_id)) {
    const double at = ms_at(ui_.in.mx);
    if (ruler_drag_ == 1 && fabsf(ui_.in.mx - ruler_press_x_) > 4) ruler_drag_ = 2;  // a drag: new loop
    if (ruler_drag_ == 1) {
      seek_dragging_ = true;
      seek_preview_ms_ = at;
    } else if (ruler_drag_ == 2) {
      seek_dragging_ = false;
      const double a = snap(std::min(ruler_anchor_ms_, at)), b = snap(std::max(ruler_anchor_ms_, at));
      midi_synth_->set_loop(true, a, std::max(b, a + 20));
    } else if (ruler_drag_ == 3) {
      midi_synth_->set_loop(loop_on, std::min(snap(at), loop_b - 20), loop_b);
    } else if (ruler_drag_ == 4) {
      midi_synth_->set_loop(loop_on, loop_a, std::max(snap(at), loop_a + 20));
    } else if (ruler_drag_ == 5 && marker_drag_ >= 0 && marker_drag_ < int(markers_.size()) &&
               fabsf(ui_.in.mx - ruler_press_x_) > 3) {
      markers_[marker_drag_].ms = snap(at);
    }
    if (!ui_.in.down) {  // released
      if (ruler_drag_ == 1) Seek(at);
      if (ruler_drag_ == 5 && marker_drag_ >= 0 && marker_drag_ < int(markers_.size())) {
        if (fabsf(ui_.in.mx - ruler_press_x_) <= 3) Seek(markers_[marker_drag_].ms);  // a click jumps there
        std::sort(markers_.begin(), markers_.end(), [](const Marker& p, const Marker& q) { return p.ms < q.ms; });
      }
      seek_dragging_ = false;
      ruler_drag_ = 0;
      marker_drag_ = -1;
      ui_.active = 0;
    }
  }
  if (hover_marker >= 0 && ruler_drag_ == 0) ui_.Tooltip("Click: jump  \xC2\xB7  drag: move  \xC2\xB7  right-click: delete",
                                                         ui_.in.mx, marker_lane.y);
}

void ClientMainWindow::DrawLoopAndPlayhead(const Rect& ruler, const Rect& lanes, double view_start_ms, double ms_per_px) {
  Gfx& g = gfx_;
  auto x_of = [&](double ms) { return lanes.x + float((ms - view_start_ms) / ms_per_px); };
  const double loop_a = midi_synth_->loop_start_ms(), loop_b = midi_synth_->loop_end_ms();
  if (midi_synth_->loop_on() && loop_b - loop_a >= 20) {
    const float la = std::max(lanes.x, x_of(loop_a)), lb = std::min(lanes.r(), x_of(loop_b));
    if (lb > la) g.Fill(Rect(la, lanes.y, lb - la, lanes.h), WithAlpha(theme::kAccent, 0.05f));
  }
  for (auto& m : markers_) {
    const float mx = x_of(m.ms);
    if (mx >= lanes.x && mx <= lanes.r()) g.Line(mx, lanes.y, mx, lanes.b(), WithAlpha(theme::kYellow, 0.35f), 1);
  }
  const float px = x_of(PlayheadMs());
  if (px >= lanes.x && px <= lanes.r()) {
    const Color c = seek_dragging_ ? theme::kYellow : recording_ ? theme::kRed : theme::kText;
    g.Line(px, ruler.y + 13, px, lanes.b(), WithAlpha(c, 0.9f), 1.5f);
    g.FillTriangle(px - 6, ruler.y + 13, px + 6, ruler.y + 13, px, ruler.y + 21, c);
  }
}

void ClientMainWindow::DrawTrackView(const Rect& area) {
  Gfx& g = gfx_;
  const SongInfo& song = midi_synth_->song();
  const Rect header(area.x, area.y, area.w, 32);
  g.Fill(header, theme::kPanel);
  g.Text("TRACKS", Rect(header.x + 16, header.y, 80, header.h), theme::kDim, 11, Align::kLeft, true);

  const Rect body(area.x, header.b(), area.w, area.h - header.h);
  if (!song.loaded) {
    g.Fill(body, theme::kBg);
    g.Text("Open a MIDI file, or start a new song", Rect(body.x, body.cy() - 44, body.w, 24), theme::kDim, 15, Align::kCenter);
    g.Text("Live playing (MIDI keyboard or the PC keys) works either way.", Rect(body.x, body.cy() - 20, body.w, 18),
           theme::kFaint, 12, Align::kCenter);
    if (ui_.Button(Id(this, 24), Rect(body.cx() - 146, body.cy() + 10, 140, 32), "Open MIDI file...", true))
      PromptAndOpen();
    if (ui_.Button(Id(this, 25), Rect(body.cx() + 6, body.cy() + 10, 140, 32), "New song")) NewSong();
    return;
  }

  // Header: transport options on the left, view controls on the right.
  float hx = header.x + 74;
  auto hbutton = [&](int salt, float w, const char* label, bool on, const char* tip) {
    const Rect br(hx, header.y + 5, w, 22);
    const bool clicked = ui_.Button(Id(this, salt), br, label, on, theme::kAccent, 11);
    if (tip && ui_.Hover(br)) ui_.Tooltip(tip, br.cx(), br.y);
    hx += w + 4;
    return clicked;
  };
  if (hbutton(26, 46, "Loop", midi_synth_->loop_on(), "Loop the region (drag on the ruler to set it)  Ctrl+L"))
    ToggleLoop();
  if (hbutton(27, 48, "Click", midi_synth_->metronome(), "Metronome on every beat"))
    midi_synth_->set_metronome(!midi_synth_->metronome());
  if (hbutton(28, 68, "Count-in", count_in_, "One bar of clicks before recording starts")) count_in_ = !count_in_;
  static const dsp::audio::synth::effects::ParamInfo kSpeed = {"Speed", 0.5f, 1.5f, 1.0f,
                                                                dsp::audio::synth::effects::ParamUnit::kPercent, false};
  if (ui_.ValueBox(Rect(hx, header.y + 5, 92, 22), kSpeed, &speed_param_, theme::kAccent, "Speed"))
    midi_synth_->set_speed(speed_param_.load());
  hx += 96;
  if (hbutton(29, 70, "+ Marker", false, "Marker at the playhead  Ctrl+M")) AddMarker();
  if (hbutton(34, 64, "+ Track", false, "New empty track to draw notes into")) OpenEditor(AddTrack());
  if (hbutton(35, 56, "Touch", touch_mode_, "Automation: moving a control while playing records it into its lane")) {
    touch_mode_ = !touch_mode_;
    if (touch_mode_) ShowMessage("Touch on: move a knob or fader while playing to record automation (right-click a control for more)");
  }
  if (area.w > 1060) {
    char info[96];
    snprintf(info, sizeof(info), "%d tracks  \xC2\xB7  %d notes", int(visible_tracks_.size()), song.TotalNotes());
    g.Text(info, Rect(hx + 8, header.y, 200, header.h), theme::kFaint, 11);
  }
  float bx = header.r() - 12;
  bx -= 30;
  if (ui_.Button(Id(this, 20), Rect(bx, header.y + 5, 26, 22), "+")) ms_per_px_ *= 0.7;
  bx -= 30;
  if (ui_.Button(Id(this, 21), Rect(bx, header.y + 5, 26, 22), "\xE2\x88\x92")) ms_per_px_ /= 0.7;
  bx -= 52;
  if (ui_.Button(Id(this, 22), Rect(bx, header.y + 5, 48, 22), "Fit", false, theme::kAccent, 11)) ZoomToFit();
  bx -= 70;
  if (ui_.Button(Id(this, 23), Rect(bx, header.y + 5, 66, 22), "Follow", follow_playhead_, theme::kAccent, 11))
    follow_playhead_ = !follow_playhead_;

  const float head_w = 236, ruler_h = 34, hbar_h = 12;
  const Rect ruler(body.x + head_w, body.y, body.w - head_w, ruler_h);
  const Rect lanes(ruler.x, ruler.b(), ruler.w, body.h - ruler_h - hbar_h);
  const Rect hbar(lanes.x, lanes.b(), lanes.w, hbar_h);
  lanes_w_ = lanes.w;
  const int take_rows = recording_ ? 1 : 0;
  const int track_rows_end = int(visible_tracks_.size()) + take_rows;
  const int lane_rows = int(song.automation.size());
  const int rows = track_rows_end + lane_rows;
  const float row_h = rows ? std::clamp(lanes.h / rows, 40.0f, 76.0f) : 60.0f;
  const float content_h = row_h * rows;
  const double pos = PlayheadMs();
  const double length = std::max({30000.0, song.length_ms, recording_ ? pos + 10000.0 : 0.0});
  ms_per_px_ = std::clamp(ms_per_px_, 0.5, length / 100.0);

  // Wheel: scroll rows, Shift = scroll time, Ctrl = zoom around the mouse.
  const float wheel = ui_.TakeWheel(Rect(body.x, body.y, body.w, ruler_h + lanes.h));
  if (wheel != 0) {
    if (ui_.in.ctrl) {
      const double at = view_start_ms_ + (ui_.in.mx - lanes.x) * ms_per_px_;
      ms_per_px_ = std::clamp(ms_per_px_ * pow(0.8, wheel), 0.5, length / 100.0);
      view_start_ms_ = at - (ui_.in.mx - lanes.x) * ms_per_px_;
    } else if (ui_.in.shift || content_h <= lanes.h) {
      view_start_ms_ -= wheel * 90 * ms_per_px_;
    } else {
      track_scroll_y_ -= wheel * row_h;
    }
  }
  track_scroll_y_ = std::clamp(track_scroll_y_, 0.0f, std::max(0.0f, content_h - lanes.h));
  const double view_ms = lanes.w * ms_per_px_;

  // Follow the playhead while playing.
  const bool playing = synth_player_->state() == dsp::audio::synth::RealtimePlayer::kStatePlaying;
  if (follow_playhead_ && playing && !seek_dragging_ && ruler_drag_ == 0 && ui_.active != Id(this, 31) &&
      (pos > view_start_ms_ + view_ms * 0.92 || pos < view_start_ms_))
    view_start_ms_ = pos - view_ms * 0.08;
  view_start_ms_ = std::clamp(view_start_ms_, -view_ms * 0.02, std::max(0.0, length - view_ms * 0.5));
  auto x_of = [&](double ms) { return lanes.x + float((ms - view_start_ms_) / ms_per_px_); };
  const double view_end = view_start_ms_ + view_ms;

  // Ruler and lane grid
  g.Fill(Rect(body.x, body.y, head_w, ruler_h), theme::kPanel);
  g.Fill(lanes, theme::kBg);
  if (song.beats.size() > 1) {
    double bar_ms = 2000;
    for (size_t i = 1; i < song.beats.size(); ++i)
      if (song.beats[i].beat == 0) { bar_ms = song.beats[i].ms - song.beats[0].ms; break; }
    const float bar_px = float(bar_ms / ms_per_px_);
    int bar_step = 1;
    while (bar_px * bar_step < 46 && bar_step < 4096) bar_step *= 2;
    const bool show_beats = bar_px / 4 > 9;
    g.PushClip(lanes);
    for (const auto& b : song.beats) {
      if (b.ms < view_start_ms_ - 1) continue;
      if (b.ms > view_end) break;
      const float lx = x_of(b.ms);
      if (b.beat == 0)
        g.Line(lx, lanes.y, lx, lanes.b(), (b.bar - 1) % bar_step == 0 ? theme::kLine : Mix(theme::kLine, theme::kBg, 0.5f), 1);
      else if (show_beats)
        g.Line(lx, lanes.y, lx, lanes.b(), Mix(theme::kPanel, theme::kBg, 0.4f), 1);
    }
    g.PopClip();
  }
  DrawRuler(ruler, view_start_ms_, ms_per_px_, lanes);

  // Rows
  bool any_track_solo = false;
  for (int t : visible_tracks_) any_track_solo |= midi_synth_->track_solo(t);
  ui_.Clip(Rect(body.x, lanes.y, body.w, lanes.h));
  if (recording_) {  // the take so far, live
    const float y = lanes.y - track_scroll_y_;
    const Rect head(body.x, y, head_w, row_h);
    g.Fill(head, Mix(theme::kPanel, theme::kRed, 0.12f));
    g.Fill(Rect(head.x, y + 1, 4, row_h - 2), theme::kRed);
    g.Circle(head.x + 20, y + 13, 5, theme::kRed, true);
    g.Text(midi_synth_->counting_in() ? "Count-in..." : "Recording", Rect(head.x + 32, y + 4, head_w - 40, 18), theme::kRed,
           12.5f, Align::kLeft, true);
    char detail[64];
    size_t held = 0;
    for (auto& row : take_open_) for (double v : row) held += v >= 0;
    snprintf(detail, sizeof(detail), "%zu notes so far", take_notes_.size() + held);
    g.Text(detail, Rect(head.x + 14, y + 22, head_w - 22, 16), theme::kDim, 10.5f);
    const Rect lane(lanes.x, y, lanes.w, row_h);
    g.Fill(lane, WithAlpha(theme::kRed, 0.06f));
    g.PushClip(lane);
    auto draw = [&](double a, double b, int note) {
      const float x0 = x_of(a), x1 = std::max(x0 + 2.0f, x_of(b));
      const float ny = y + row_h - 5 - (note - 24) * (row_h - 10) / 84.0f;
      g.Fill(Rect(x0, ny - 1.5f, x1 - x0, 3), theme::kRed);
    };
    for (auto& t : take_notes_) draw(t.start_ms, t.end_ms, t.note);
    for (int ch = 0; ch < 16; ++ch)
      for (int n = 0; n < 128; ++n)
        if (take_open_[ch][n] >= 0) draw(take_open_[ch][n], pos, n);
    g.PopClip();
    g.Line(body.x, y + row_h - 0.5f, lanes.r(), y + row_h - 0.5f, Mix(theme::kLine, theme::kBg, 0.4f));
  }
  for (int row = take_rows; row < track_rows_end; ++row) {
    const float y = lanes.y + row * row_h - track_scroll_y_;
    if (y > lanes.b() || y + row_h < lanes.y) continue;
    const int ti = visible_tracks_[row - take_rows];
    const SongTrack& track = song.tracks[ti];
    const int ch = FirstChannel(track.channel_mask);
    const Color color = theme::Channel(ch);
    const bool muted = midi_synth_->track_muted(ti);
    const bool soloed = midi_synth_->track_solo(ti);
    const bool selected = selected_strip_ == ch;
    const bool audible = !muted && (!any_track_solo || soloed);

    // header
    const Rect head(body.x, y, head_w, row_h);
    g.Fill(head, selected ? theme::kPanel2 : theme::kPanel);
    g.Fill(Rect(head.x, y + 1, 4, row_h - 2), color);
    char name[128];
    snprintf(name, sizeof(name), "%d  %s", ti + 1, track.name.empty() ? "(untitled)" : track.name.c_str());
    const float btn_w = 24, edit_w = 34;
    g.Text(name, Rect(head.x + 14, y + 4, head_w - 14 - 2 * btn_w - edit_w - 20, 18), muted ? theme::kFaint : theme::kText,
           12.5f, Align::kLeft, true);
    char detail[128];
    int nch = 0;
    for (int c = 0; c < 16; ++c) nch += (track.channel_mask >> c) & 1;
    snprintf(detail, sizeof(detail), "Ch %d%s  \xC2\xB7  %s  \xC2\xB7  %zu", ch + 1, nch > 1 ? "+" : "",
             track.program >= 0 ? ProgramName(track.program) : ProgramName(ch == 9 ? 128 : 0), track.notes.size());
    if (row_h >= 40)
      g.Text(detail, Rect(head.x + 14, y + 22, head_w - 22, 16), theme::kDim, 10.5f);
    const Rect edit_r(head.r() - 2 * btn_w - edit_w - 16, y + 5, edit_w, 18);
    const Rect mute_r(head.r() - 2 * btn_w - 12, y + 5, btn_w, 18), solo_r(head.r() - btn_w - 8, y + 5, btn_w, 18);
    if (ui_.Button(Id(&track, 4), edit_r, "Edit", false, theme::kAccent, 10.5f)) OpenEditor(ti);
    if (ui_.Hover(edit_r)) ui_.Tooltip("Edit the notes (piano roll)  -  or double-click the track", edit_r.cx(), edit_r.y);
    if (ui_.Button(Id(&track, 1), mute_r, "M", muted, theme::kOrange, 11)) midi_synth_->set_track_muted(ti, !muted);
    if (ui_.Button(Id(&track, 2), solo_r, "S", soloed, theme::kYellow, 11)) midi_synth_->set_track_solo(ti, !soloed);
    if (ui_.Hover(head) && ui_.in.dbl && ui_.in.pressed) {
      ui_.in.pressed = false;
      OpenEditor(ti);
    }
    if (ui_.Clicked(Id(&track, 3), head)) selected_strip_ = ch;

    // lane
    const Rect lane(lanes.x, y, lanes.w, row_h);
    if (row & 1) g.Fill(lane, WithAlpha(theme::kPanel, 0.35f));
    g.Line(body.x, y + row_h - 0.5f, lanes.r(), y + row_h - 0.5f, Mix(theme::kLine, theme::kBg, 0.4f));
    if (track.notes.empty()) {
      g.Text("empty - click Edit to draw notes, or record", Rect(lanes.x + 12, y, 400, row_h), theme::kFaint, 11);
      continue;
    }
    int lo = track.low_note, hi = track.high_note;
    if (hi - lo < 12) {
      const int mid = (lo + hi) / 2;
      lo = mid - 6;
      hi = mid + 6;
    }
    const float pad = 5;
    const float note_h = std::max(1.5f, (row_h - 2 * pad) / float(hi - lo + 1));
    const float alpha = audible ? 1.0f : 0.25f;
    g.PushClip(lane);
    for (const auto& n : track.notes) {
      if (n.start_ms > view_end) break;
      if (n.end_ms < view_start_ms_) continue;
      const float x0 = x_of(n.start_ms), x1 = std::max(x0 + 1.5f, x_of(n.end_ms));
      const float ny = y + row_h - pad - (n.note - lo + 1) * note_h;
      const bool sounding = playing && n.start_ms <= pos && n.end_ms > pos;
      Color c = theme::Channel(n.channel);
      c = sounding ? Mix(c, Rgb(0xffffff), 0.55f) : c;
      c.a = alpha * (0.45f + 0.55f * n.velocity / 127.0f);
      g.Fill(Rect(x0, ny, x1 - x0 - (x1 - x0 > 3 ? 0.5f : 0.0f), std::max(1.5f, note_h - (note_h > 3 ? 1.0f : 0.0f))), c);
    }
    g.PopClip();
  }
  // automation lanes, under the tracks
  for (int li = 0; li < lane_rows; ++li) {
    const float y = lanes.y + (track_rows_end + li) * row_h - track_scroll_y_;
    if (y > lanes.b() || y + row_h < lanes.y) continue;
    g.Line(body.x, y + row_h - 0.5f, lanes.r(), y + row_h - 0.5f, Mix(theme::kLine, theme::kBg, 0.4f));
    DrawAutomationRow(Rect(body.x, y, head_w, row_h - 1), Rect(lanes.x, y, lanes.w, row_h - 1), li, view_start_ms_, ms_per_px_);
  }
  if (rows == 0)
    g.Text("Empty song: press \xE2\x97\x8F Record (Ctrl+R) to record from a MIDI keyboard or the PC keys, or + Track to draw notes",
           Rect(lanes.x + 16, lanes.y + 10, lanes.w - 32, 24), theme::kDim, 12);
  ui_.Unclip();
  g.Line(lanes.x - 0.5f, ruler.y, lanes.x - 0.5f, lanes.b(), theme::kLine);

  // Seek: click or drag in the lanes; the jump happens on release.
  const WidgetId seek_id = Id(this, 30);
  auto ms_at = [&](float px) { return std::max(0.0, view_start_ms_ + (px - lanes.x) * ms_per_px_); };
  if (ui_.Press(seek_id, lanes)) {
    seek_dragging_ = true;
    seek_preview_ms_ = ms_at(ui_.in.mx);
  }
  if (ui_.active == seek_id && seek_dragging_) {
    seek_preview_ms_ = ms_at(ui_.in.mx);
    if (!ui_.in.down) {
      seek_dragging_ = false;
      ui_.active = 0;
      Seek(seek_preview_ms_);
    }
  }
  DrawLoopAndPlayhead(ruler, lanes, view_start_ms_, ms_per_px_);

  // Horizontal scrollbar
  g.Fill(Rect(body.x, hbar.y, body.w, hbar.h), theme::kPanel);
  const float thumb_w = std::max(30.0f, float(hbar.w * std::min(1.0, view_ms / length)));
  const float thumb_x = hbar.x + float((hbar.w - thumb_w) * std::clamp(view_start_ms_ / std::max(1.0, length - view_ms), 0.0, 1.0));
  const Rect thumb(thumb_x, hbar.y + 3, thumb_w, hbar.h - 6);
  const WidgetId bar_id = Id(this, 31);
  if (ui_.Press(bar_id, hbar)) hbar_grab_ = thumb.Contains(ui_.in.mx, ui_.in.my) ? ui_.in.mx - thumb_x : thumb_w * 0.5f;
  if (ui_.Dragging(bar_id)) {
    const float t = std::clamp((ui_.in.mx - hbar_grab_ - hbar.x) / std::max(1.0f, hbar.w - thumb_w), 0.0f, 1.0f);
    view_start_ms_ = t * std::max(0.0, length - view_ms);
  }
  g.FillRound(thumb, 3, ui_.active == bar_id || ui_.Hover(hbar) ? theme::kFaint : theme::kPanel3);

  // Vertical scroll indicator
  if (content_h > lanes.h) {
    const float th = lanes.h * lanes.h / content_h;
    const float ty = lanes.y + (lanes.h - th) * track_scroll_y_ / (content_h - lanes.h);
    g.FillRound(Rect(lanes.r() - 5, ty, 3, th), 1.5f, theme::kFaint);
  }
}

// ---------------------------------------------------------------------------------------------
// Mixer
// ---------------------------------------------------------------------------------------------
void ClientMainWindow::DrawMeter(const Rect& r, const LevelMeter& m) {
  Gfx& g = gfx_;
  const float gap = 2;
  const float bw = (r.w - gap) * 0.5f;
  auto y_of = [&](float db) { return r.b() - r.h * std::clamp((db + 60.0f) / 66.0f, 0.0f, 1.0f); };
  for (int c = 0; c < 2; ++c) {
    const Rect bar(r.x + c * (bw + gap), r.y, bw, r.h);
    g.Fill(bar, Rgb(0x0b0c10));
    const float top = y_of(m.db[c]);
    if (m.db[c] > -60) {
      const float y_yellow = y_of(-12), y_red = y_of(-3);
      g.Fill(Rect(bar.x, std::max(top, y_yellow), bw, bar.b() - std::max(top, y_yellow)), theme::kGreen);
      if (top < y_yellow) g.Fill(Rect(bar.x, std::max(top, y_red), bw, y_yellow - std::max(top, y_red)), theme::kYellow);
      if (top < y_red) g.Fill(Rect(bar.x, top, bw, y_red - top), theme::kRed);
    }
    if (m.hold[c] > -60) {
      const float hy = y_of(m.hold[c]);
      g.Fill(Rect(bar.x, hy - 1, bw, 1.5f), m.hold[c] > -3 ? theme::kRed : theme::kText);
    }
  }
  if (m.gr_db < -0.1f) {  // gain reduction hangs down from the top in orange
    const float h = r.h * std::min(1.0f, -m.gr_db / 24.0f);
    g.Fill(Rect(r.x, r.y, r.w, h), WithAlpha(theme::kOrange, 0.8f));
  }
}

void ClientMainWindow::DrawMixer(const Rect& area) {
  Gfx& g = gfx_;
  g.Fill(area, theme::kPanel);
  g.Line(area.x, area.y + 0.5f, area.r(), area.y + 0.5f, theme::kLine);
  g.Text("MIXER", Rect(area.x + 16, area.y, 80, 28), theme::kDim, 11, Align::kLeft, true);
  auto& mixer = midi_synth_->mixer;
  if (mixer.AnySolo()) {
    g.FillRound(Rect(area.x + 74, area.y + 7, 40, 15), 3, theme::kYellow);
    g.Text("SOLO", Rect(area.x + 74, area.y + 7, 40, 15), Rgb(0x0d0f13), 9.5f, Align::kCenter, true);
  }
  // view options
  if (ui_.Button(Id(this, 43), Rect(area.x + 124, area.y + 4, 78, 20), "Used only", mixer_used_only_, theme::kAccent, 10.5f))
    mixer_used_only_ = !mixer_used_only_;
  if (ui_.Hover(Rect(area.x + 124, area.y + 4, 78, 20)))
    ui_.Tooltip("Show only channels the song uses (or that are playing / selected)", area.x + 163, area.y + 4);
  if (ui_.Button(Id(this, 44), Rect(area.x + 206, area.y + 4, 52, 20), "Wide", mixer_wide_, theme::kAccent, 10.5f))
    mixer_wide_ = !mixer_wide_;
  if (area.w > 1020)
    g.Text("click a strip to edit its chain  \xC2\xB7  right-click a knob to automate / MIDI-learn",
           Rect(area.x + 268, area.y, 420, 28), theme::kFaint, 11);

  // Follow the file's reverb/chorus send levels (GM CC91/CC93) on top of the send knobs.
  const bool follow = mixer.follow_midi_sends();
  const Rect follow_r(area.r() - 196, area.y + 4, 186, 20);
  if (ui_.Button(Id(this, 41), follow_r, follow ? "File reverb/chorus: on" : "File reverb/chorus: off", follow,
                 theme::kAccent, 10.5f))
    mixer.set_follow_midi_sends(!follow);
  if (ui_.Hover(follow_r))
    ui_.Tooltip("Add the reverb and chorus levels the MIDI file asks for (CC91/CC93) to each strip's sends",
                follow_r.cx(), follow_r.y);
  const Rect reset_r(follow_r.x - 84, follow_r.y, 78, 20);
  if (ui_.Button(Id(this, 42), reset_r, "Reset all", false, theme::kAccent, 10.5f)) mixer.ResetAll();
  if (ui_.Hover(reset_r)) ui_.Tooltip("Every strip and the master back to defaults", reset_r.cx(), reset_r.y);

  const float y0 = area.y + 28, h = area.h - 28 - 8;
  const float master_w = 96;
  const float avail = area.w - master_w - 28;
  int shown[16], count = 0;
  for (int ch = 0; ch < 16; ++ch)
    if (!mixer_used_only_ || StripUsed(ch)) shown[count++] = ch;
  const float sw = count ? std::clamp(avail / count, 40.0f, mixer_wide_ ? 130.0f : 84.0f) : 84.0f;
  for (int i = 0; i < count; ++i)
    DrawStrip(Rect(area.x + 10 + i * sw, y0, sw - 4, h), shown[i]);
  if (count == 0)
    g.Text("no channels in use yet", Rect(area.x + 10, y0, avail, 40), theme::kFaint, 12);
  DrawMasterStrip(Rect(area.r() - master_w - 10, y0, master_w, h));
}

void ClientMainWindow::DrawStrip(const Rect& r, int ch) {
  Gfx& g = gfx_;
  auto& p = midi_synth_->mixer.strip(ch);
  const Color color = theme::Channel(ch);
  const bool selected = selected_strip_ == ch;
  const bool named = !strip_names_[ch].empty();

  g.FillRound(r, 5, selected ? theme::kPanel3 : theme::kPanel2);
  if (selected) g.StrokeRound(r, 5, color, 1.5f);
  float y = r.y + 4;
  const Rect chip(r.x + 3, y, r.w - 6, 19);
  g.FillRound(chip, 3, WithAlpha(color, selected ? 0.9f : named ? 0.32f : 0.14f));
  char name[96];
  snprintf(name, sizeof(name), "%d %s", ch + 1, named ? strip_names_[ch].c_str() : "");
  g.Text(name, chip.Inset(4, 0), selected ? Rgb(0x0d0f13) : theme::kText, 11, Align::kLeft, true);
  y += 22;
  const bool fixed = midi_synth_->channel_override(ch) >= 0;  // instrument picked in the studio
  g.Text(ProgramName(midi_synth_->channel_program(ch)), Rect(r.x + 3, y, r.w - 6, 13), fixed ? color : theme::kFaint,
         9.5f, Align::kCenter, fixed);
  y += 16;

  // Section indicators: input filters/drive, EQ, dynamics, sends
  const bool pre = p.get(kStripHpfOn) >= 0.5f || p.get(kStripLpfOn) >= 0.5f || p.get(kStripDriveOn) >= 0.5f ||
                   fabsf(p.get(kStripTrim)) > 0.05f || p.get(kStripPhase) >= 0.5f;
  const bool eq = p.get(kStripEq + kEqOn) >= 0.5f;
  const bool dyn = p.get(kStripComp + kCompOn) >= 0.5f;
  auto& mixer = midi_synth_->mixer;
  const bool file_fx = named && mixer.follow_midi_sends() &&
                       (mixer.midi_send(ch, Mixer::kBusReverb) > 0 || mixer.midi_send(ch, Mixer::kBusChorus) > 0);
  const bool fx = p.get(kStripSendReverb) > 0 || p.get(kStripSendDelay) > 0 || p.get(kStripSendChorus) > 0 || file_fx;
  // F = input filters/drive, E = EQ, C = compressor, S = FX sends
  const char* labels[4] = {"F", "E", "C", "S"};
  const bool lit[4] = {pre, eq, dyn, fx};
  const float iw = (r.w - 6) / 4.0f;
  for (int i = 0; i < 4; ++i) {
    const Rect ir(r.x + 3 + i * iw + 1, y, iw - 2, 14);
    g.FillRound(ir, 2, lit[i] ? WithAlpha(color, 0.85f) : theme::kPanel);
    if (lit[i] || selected) g.Text(labels[i], ir, lit[i] ? Rgb(0x0d0f13) : theme::kFaint, 8.5f, Align::kCenter, true);
  }
  y += 18;

  ui_.Knob(Rect(r.x, y, r.w, 34), StripParamInfo(kStripPan), p.slot(kStripPan), color, "", false);
  y += 36;

  const float bottom_h = 40;
  const Rect fa(r.x + 3, y, r.w - 6, r.b() - y - bottom_h);
  const float fader_w = std::min(26.0f, fa.w * 0.55f);
  ui_.Fader(Rect(fa.x, fa.y, fader_w, fa.h), StripParamInfo(kStripVolume), p.slot(kStripVolume), color);
  DrawMeter(Rect(fa.x + fader_w + 4, fa.y + 8, std::min(14.0f, fa.w - fader_w - 6), fa.h - 16), strip_meter_[ch]);

  char text[32];
  FormatParam(StripParamInfo(kStripVolume), p.get(kStripVolume), text, sizeof(text));
  if (strncmp(text, "-inf", 4) != 0) text[strlen(text) - 3] = 0;  // drop " dB"
  g.Text(text, Rect(r.x, fa.b() + 1, r.w, 14), theme::kDim, 10, Align::kCenter, false, true);
  const float bw = (r.w - 9) * 0.5f;
  ui_.Switch(Id(p.slot(kStripMute), 1), Rect(r.x + 3, r.b() - 22, bw, 18), "M", p.slot(kStripMute), theme::kOrange);
  ui_.Switch(Id(p.slot(kStripSolo), 1), Rect(r.x + 6 + bw, r.b() - 22, bw, 18), "S", p.slot(kStripSolo), theme::kYellow);

  if (ui_.Clicked(Id(&strip_names_[ch], 9), r)) selected_strip_ = ch;
}

void ClientMainWindow::DrawMasterStrip(const Rect& r) {
  Gfx& g = gfx_;
  auto& m = midi_synth_->mixer.master();
  const bool selected = selected_strip_ < 0;
  g.FillRound(r, 5, selected ? theme::kPanel3 : theme::kPanel2);
  if (selected) g.StrokeRound(r, 5, theme::kText, 1.5f);
  float y = r.y + 4;
  const Rect chip(r.x + 3, y, r.w - 6, 19);
  g.FillRound(chip, 3, selected ? theme::kText : Rgb(0x3a404c));
  g.Text("MASTER", chip, selected ? Rgb(0x0d0f13) : theme::kText, 11, Align::kCenter, true);
  y += 22;
  g.Text("stereo bus", Rect(r.x, y, r.w, 13), theme::kFaint, 9.5f, Align::kCenter);
  y += 16;
  const bool eq = m.get(kMasterEq + kEqOn) >= 0.5f, comp = m.get(kMasterComp + kCompOn) >= 0.5f;
  const float iw = (r.w - 6) / 2.0f;
  const char* labels[2] = {"EQ", "COMP"};
  const bool lit[2] = {eq, comp};
  for (int i = 0; i < 2; ++i) {
    const Rect ir(r.x + 3 + i * iw + 1, y, iw - 2, 14);
    g.FillRound(ir, 2, lit[i] ? theme::kText : theme::kPanel);
    g.Text(labels[i], ir, lit[i] ? Rgb(0x0d0f13) : theme::kFaint, 8.5f, Align::kCenter, true);
  }
  y += 18;
  ui_.Knob(Rect(r.x, y, r.w, 34), MasterParamInfo(kMasterWidth), m.slot(kMasterWidth), theme::kText, "", false);
  y += 36;
  const float bottom_h = 40;
  const Rect fa(r.x + 6, y, r.w - 12, r.b() - y - bottom_h);
  ui_.Fader(Rect(fa.x, fa.y, 28, fa.h), MasterParamInfo(kMasterGain), m.slot(kMasterGain), theme::kText);
  DrawMeter(Rect(fa.x + 34, fa.y + 8, fa.w - 34, fa.h - 16), master_meter_);
  char text[32];
  FormatParam(MasterParamInfo(kMasterGain), m.get(kMasterGain), text, sizeof(text));
  g.Text(text, Rect(r.x, fa.b() + 1, r.w, 14), theme::kDim, 10, Align::kCenter, false, true);
  snprintf(text, sizeof(text), "LIM %.1f", limiter_db_);
  g.Text(text, Rect(r.x, r.b() - 22, r.w, 18), limiter_db_ < -0.5f ? theme::kOrange : theme::kFaint, 10, Align::kCenter,
         false, true);
  if (ui_.Clicked(Id(this, 40), r)) selected_strip_ = -1;
}

// ---------------------------------------------------------------------------------------------
// Inspector
// ---------------------------------------------------------------------------------------------
float ClientMainWindow::DrawCard(float x, float y, float w, float body_h, const char* title, std::atomic<float>* power,
                                 Color accent) {
  Gfx& g = gfx_;
  const Rect card(x, y, w, 28 + body_h);
  g.FillRound(card, 6, theme::kPanel2);
  float tx = x + 12;
  if (power) {
    ui_.Power(Id(power, 5), x + 20, y + 14, power, accent);
    tx = x + 36;
  }
  const bool on = !power || power->load() >= 0.5f;
  g.Text(title, Rect(tx, y + 4, w - (tx - x) - 10, 20), on ? theme::kText : theme::kDim, 11, Align::kLeft, true);
  return y + 28;
}

void ClientMainWindow::DrawGainReduction(const Rect& r, float gr_db, const char* label) {
  Gfx& g = gfx_;
  g.Text(label,Rect(r.x - 4, r.y, r.w + 8, 12), theme::kDim, 10, Align::kCenter);
  const Rect bar(r.cx() - 4, r.y + 14, 8, r.h - 30);
  g.Fill(bar, Rgb(0x0b0c10));
  const float t = std::min(1.0f, -gr_db / 24.0f);
  g.Fill(Rect(bar.x, bar.y, bar.w, bar.h * t), theme::kOrange);
  char text[16];
  snprintf(text, sizeof(text), "%.1f", gr_db);
  g.Text(text, Rect(r.x - 8, bar.b() + 2, r.w + 16, 12), gr_db < -0.1f ? theme::kOrange : theme::kFaint, 10, Align::kCenter);
}

void ClientMainWindow::DrawInspector(const Rect& area) {
  Gfx& g = gfx_;
  g.Fill(area, theme::kPanel);
  g.Line(area.x + 0.5f, area.y, area.x + 0.5f, area.b(), theme::kLine);
  const int sel = selected_strip_;
  const Color color = sel >= 0 ? theme::Channel(sel) : theme::kText;

  // Header
  const Rect head(area.x, area.y, area.w, 60);
  g.Circle(head.x + 26, head.cy(), 9, color, true);
  char title[128];
  const float buttons_w = 168;
  if (sel >= 0) {
    snprintf(title, sizeof(title), "Channel %d%s%s", sel + 1, strip_names_[sel].empty() ? "" : "  \xC2\xB7  ",
             strip_names_[sel].c_str());
    // Instrument: click to pick one for this channel (or go back to the file's choice).
    const bool fixed = midi_synth_->channel_override(sel) >= 0;
    char inst[96];
    snprintf(inst, sizeof(inst), "%s%s  \xE2\x96\xBE", ProgramName(midi_synth_->channel_program(sel)),
             fixed ? "  (fixed)" : "");
    const Rect inst_r(head.x + 42, head.y + 31, std::min(head.w - 58, g.TextWidth(inst, 11, fixed) + 20), 20);
    if (ui_.Button(Id(this, 51), inst_r, "", fixed, color)) {
      picker_open_ = true;
      picker_family_ = std::min(15, std::max(0, midi_synth_->channel_program(sel) / 8));
      picker_anchor_ = Rect(area.x + 14, head.b() + 2, area.w - 28, 0);
    }
    g.Text(inst, inst_r.Inset(8, 0), fixed ? Rgb(0x0d0f13) : theme::kText, 11, Align::kLeft, fixed);
    if (ui_.Hover(inst_r)) ui_.Tooltip("Choose the instrument for this channel", inst_r.cx(), inst_r.y);
    // Calibration badge: is this instrument playing a calibration file measured from recordings?
    const std::string cal = midi_synth_->ChannelCalibration(sel);
    const char* badge = cal.empty() ? "PRESET" : "CALIBRATED";
    const Rect cal_r(inst_r.r() + 8, inst_r.y + 2, g.TextWidth(badge, 9.5f, true) + 14, 16);
    if (cal_r.r() < head.r() - 8) {
      const Color cal_color = cal.empty() ? theme::kFaint : theme::kGreen;
      g.FillRound(cal_r, 3, WithAlpha(cal_color, cal.empty() ? 0.18f : 0.85f));
      g.Text(badge, cal_r, cal.empty() ? theme::kDim : Rgb(0x0d0f13), 9.5f, Align::kCenter, true);
      if (ui_.Hover(cal_r)) {
        const std::string tip = cal.empty() ? std::string("Hand-set model: no calibration file for this instrument")
                                            : "Plays " + cal;
        ui_.Tooltip(tip.c_str(), cal_r.cx(), cal_r.y);
      }
    }
  } else {
    snprintf(title, sizeof(title), "Master bus");
    g.Text("FX returns \xE2\x86\x92 EQ \xE2\x86\x92 bus compressor \xE2\x86\x92 width \xE2\x86\x92 output \xE2\x86\x92 limiter",
           Rect(head.x + 46, head.y + 32, head.w - 60, 16), theme::kDim, 10.5f);
  }
  g.Text(title, Rect(head.x + 46, head.y + 8, head.w - 56 - buttons_w, 22), theme::kText, 15, Align::kLeft, true);
  float bx = head.r() - 14 - buttons_w;
  const char* what = sel >= 0 ? "channel chain" : "master bus";
  if (ui_.Button(Id(this, 52), Rect(bx, head.y + 8, 52, 22), "Load", false, theme::kAccent, 11)) PromptLoadChain();
  if (ui_.Hover(Rect(bx, head.y + 8, 52, 22))) {
    char tip[64];
    snprintf(tip, sizeof(tip), "Load a %s preset", what);
    ui_.Tooltip(tip, bx + 26, head.y + 8);
  }
  bx += 56;
  if (ui_.Button(Id(this, 53), Rect(bx, head.y + 8, 52, 22), "Save", false, theme::kAccent, 11)) PromptSaveChain();
  if (ui_.Hover(Rect(bx, head.y + 8, 52, 22))) {
    char tip[64];
    snprintf(tip, sizeof(tip), "Save this %s as a preset", what);
    ui_.Tooltip(tip, bx + 26, head.y + 8);
  }
  bx += 56;
  if (ui_.Button(Id(this, 50), Rect(bx, head.y + 8, 56, 22), "Reset", false, theme::kAccent, 11)) {
    if (sel >= 0) midi_synth_->mixer.ResetStrip(sel);
    else midi_synth_->mixer.ResetMaster();
  }
  g.Line(area.x, head.b() - 0.5f, area.r(), head.b() - 0.5f, theme::kLine);

  // Scrolling body
  const Rect body(area.x, head.b(), area.w, area.h - head.h);
  inspector_scroll_ = std::clamp(inspector_scroll_, 0.0f, std::max(0.0f, inspector_content_h_ - body.h));
  ui_.Clip(body);
  // While the wheel is scrolling the panel, knobs passing under the cursor must not catch it.
  const double now_s = GetTickCount64() / 1000.0;
  ui_.knob_wheel = now_s > inspector_wheel_until_;
  // The response graph takes whatever height the cards leave, so the panel fits without scrolling.
  graph_h_ = std::clamp(body.h - (sel >= 0 ? 710.0f : 900.0f), 120.0f, 240.0f);
  const float top = body.y + 12 - inspector_scroll_;
  const float end = sel >= 0 ? DrawChannelInspector(body.x + 14, top, body.w - 28, sel)
                             : DrawMasterInspector(body.x + 14, top, body.w - 28);
  inspector_content_h_ = end - top + 24;
  const float wheel = ui_.TakeWheel(body);  // whatever the knobs did not take scrolls the panel
  ui_.Unclip();
  ui_.knob_wheel = true;
  if (wheel != 0 && inspector_content_h_ > body.h) {
    inspector_scroll_ -= wheel * 60;
    inspector_wheel_until_ = now_s + 0.6;
  }
  if (inspector_content_h_ > body.h) {
    const float th = body.h * body.h / inspector_content_h_;
    const float ty = body.y + (body.h - th) * inspector_scroll_ / (inspector_content_h_ - body.h);
    g.FillRound(Rect(body.r() - 5, ty, 3, th), 1.5f, theme::kFaint);
  }
}

float ClientMainWindow::DrawChannelInspector(float x, float y, float w, int ch) {
  auto& p = midi_synth_->mixer.strip(ch);
  const Color color = theme::Channel(ch);
  auto knob = [&](float kx, float ky, float kw, int id, const char* label = nullptr) {
    ui_.Knob(Rect(kx, ky, kw, 66), StripParamInfo(id), p.slot(id), color, label);
  };
  auto toggle = [&](float kx, float ky, float kw, int id, const char* label) {
    ui_.Switch(Id(p.slot(id), 2), Rect(kx + 3, ky + 22, kw - 6, 22), label, p.slot(id), color);
  };

  DrawResponseGraph(Rect(x, y, w, graph_h_), ch);
  y += graph_h_ + 10;

  // INPUT: trim, phase, high-pass, low-pass
  float by = DrawCard(x, y, w, 72, "INPUT  \xC2\xB7  PRE", nullptr, color);
  float cw = w / 8.0f;
  knob(x, by, cw, kStripTrim);
  toggle(x + cw, by, cw, kStripPhase, "\xC3\x98 Phase");
  toggle(x + 2 * cw, by, cw, kStripHpfOn, "HPF");
  knob(x + 3 * cw, by, cw, kStripHpfFreq);
  toggle(x + 4 * cw, by, cw, kStripHpfSteep, "24 dB");
  toggle(x + 5 * cw, by, cw, kStripLpfOn, "LPF");
  knob(x + 6 * cw, by, cw, kStripLpfFreq);
  knob(x + 7 * cw, by, cw, kStripLpfQ);
  y = by + 72 + 10;

  // DRIVE
  by = DrawCard(x, y, w, 72, "DRIVE  \xC2\xB7  soft saturation", p.slot(kStripDriveOn), color);
  cw = w / 8.0f;
  knob(x, by, cw, kStripDrive);
  knob(x + cw, by, cw, kStripDriveMix);
  gfx_.Text("tanh saturation after the high-pass, before the low-pass; level-matched at -12 dBFS",
            Rect(x + 2 * cw + 8, by + 18, w - 2 * cw - 16, 30), theme::kFaint, 10.5f);
  y = by + 72 + 10;

  // EQ
  by = DrawCard(x, y, w, 86, "EQUALIZER  \xC2\xB7  4 bands", p.slot(kStripEq + kEqOn), color);
  cw = w / 12.0f;
  static const char* kBandNames[kEqBands] = {"LOW SHELF", "LO-MID", "HI-MID", "HIGH SHELF"};
  for (int b = 0; b < kEqBands; ++b) {
    const float gx = x + b * 3 * cw;
    gfx_.Text(kBandNames[b], Rect(gx, by - 2, 3 * cw, 14), kBandColor[b], 9.5f, Align::kCenter, true);
    ui_.Knob(Rect(gx, by + 14, cw, 66), StripParamInfo(kStripEq + EqBandFreq(b)), p.slot(kStripEq + EqBandFreq(b)),
             kBandColor[b], "Freq");
    ui_.Knob(Rect(gx + cw, by + 14, cw, 66), StripParamInfo(kStripEq + EqBandGain(b)), p.slot(kStripEq + EqBandGain(b)),
             kBandColor[b], "Gain");
    ui_.Knob(Rect(gx + 2 * cw, by + 14, cw, 66), StripParamInfo(kStripEq + EqBandQ(b)), p.slot(kStripEq + EqBandQ(b)),
             kBandColor[b], "Q");
  }
  y = by + 86 + 10;

  // GATE
  by = DrawCard(x, y, w, 72, "GATE  \xC2\xB7  expander", p.slot(kStripGateOn), color);
  cw = w / 8.0f;
  for (int i = 0; i < 4; ++i) knob(x + i * cw, by, cw, kStripGateThreshold + i);
  gfx_.Text("closes by Range when the level falls under Thresh; before the compressor",
            Rect(x + 4 * cw + 8, by + 18, w - 4 * cw - 16, 30), theme::kFaint, 10.5f);
  y = by + 72 + 10;

  // DYNAMICS
  by = DrawCard(x, y, w, 72, "DYNAMICS  \xC2\xB7  compressor", p.slot(kStripComp + kCompOn), color);
  cw = w / 9.2f;
  for (int i = 0; i < 6; ++i) knob(x + i * cw, by, cw, kStripComp + kCompThreshold + i);
  knob(x + 6 * cw, by, cw, kStripCompSidechain);
  if (ui_.Hover(Rect(x + 6 * cw, by, cw, 66)))
    ui_.Tooltip("Sidechain: duck this channel under another one (e.g. pads under the drums)", x + 6.5f * cw, by);
  toggle(x + 7 * cw, by, cw, kStripDynFirst, "Pre-EQ");
  DrawGainReduction(Rect(x + w - 34, by, 26, 70), strip_meter_[ch].gr_db);
  y = by + 72 + 10;

  // OUTPUT: fader, balance, sends
  char out_title[160] = "OUTPUT  \xC2\xB7  POST  \xC2\xB7  sends";
  auto& mixer = midi_synth_->mixer;
  if (mixer.follow_midi_sends())
    snprintf(out_title, sizeof(out_title), "OUTPUT  \xC2\xB7  POST  \xC2\xB7  sends   (+ file: reverb %.0f%%, chorus %.0f%%)",
             mixer.midi_send(ch, Mixer::kBusReverb) * 100, mixer.midi_send(ch, Mixer::kBusChorus) * 100);
  by = DrawCard(x, y, w, 72, out_title, nullptr, color);
  cw = w / 7.0f;
  knob(x, by, cw, kStripVolume);
  knob(x + cw, by, cw, kStripPan);
  knob(x + 2.2f * cw, by, cw, kStripSendReverb);
  knob(x + 3.2f * cw, by, cw, kStripSendDelay);
  knob(x + 4.2f * cw, by, cw, kStripSendChorus);
  toggle(x + 5.4f * cw, by, cw * 1.5f, kStripSendPre, "Pre-fader");
  y = by + 72;
  return y;
}

float ClientMainWindow::DrawMasterInspector(float x, float y, float w) {
  auto& m = midi_synth_->mixer.master();
  const Color color = theme::kText;
  auto knob = [&](float kx, float ky, float kw, int id, Color c = theme::kAccent) {
    ui_.Knob(Rect(kx, ky, kw, 66), MasterParamInfo(id), m.slot(id), c);
  };

  DrawResponseGraph(Rect(x, y, w, graph_h_), -1);
  y += graph_h_ + 10;

  float cw = w / 7.0f;
  auto& mixer = midi_synth_->mixer;
  const bool convolution = m.get(kRevMode) >= 0.5f;
  float by = DrawCard(x, y, w, 104, convolution ? "REVERB  \xC2\xB7  convolution  \xC2\xB7  send bus"
                                                : "REVERB  \xC2\xB7  algorithmic  \xC2\xB7  send bus", nullptr, color);
  knob(x, by, cw, kRevSize, convolution ? theme::kFaint : theme::kAccent);
  knob(x + cw, by, cw, kRevDamp, convolution ? theme::kFaint : theme::kAccent);
  knob(x + 2 * cw, by, cw, kRevWidth, theme::kAccent);
  knob(x + 3 * cw, by, cw, kRevPreDelay, theme::kAccent);
  knob(x + 4.3f * cw, by, cw, kRevReturn, theme::kText);
  {
    // engine and impulse response
    float bx = x + 6;
    const float by2 = by + 72;
    if (ui_.Button(Id(this, 110), Rect(bx, by2, 92, 22), "Algorithmic", !convolution, theme::kAccent, 11))
      m.set(kRevMode, 0);
    bx += 96;
    if (ui_.Button(Id(this, 111), Rect(bx, by2, 92, 22), "Convolution", convolution, theme::kAccent, 11))
      m.set(kRevMode, 1);
    bx += 104;
    const int ir = int(lroundf(m.get(kRevIr)));
    for (int i = 0; i < 3; ++i) {
      if (ui_.Button(Id(this, 112 + i), Rect(bx, by2, 50, 22), PresetIrName(i), convolution && ir == i, theme::kAccent, 11)) {
        mixer.SetConvolutionPreset(i);
        m.set(kRevIr, float(i));
        m.set(kRevMode, 1);
        ir_path_.clear();
      }
      bx += 54;
    }
    if (ui_.Button(Id(this, 115), Rect(bx, by2, 60, 22), "Load...", convolution && ir == 3, theme::kAccent, 11))
      PromptLoadIr();
    if (ui_.Hover(Rect(bx, by2, 60, 22)))
      ui_.Tooltip("Load an impulse response recorded in a real space (WAV)", bx + 30, by2);
    if (convolution) {
      char info[160];
      snprintf(info, sizeof(info), "%s  %.1f s", mixer.convolution_ir_name().c_str(), mixer.convolution_ir_seconds());
      gfx_.Text(info, Rect(x + 5.4f * cw, by + 14, w - 5.4f * cw - 8, 40), theme::kDim, 10.5f);
    }
  }
  y = by + 104 + 10;

  by = DrawCard(x, y, w, 72, "DELAY  \xC2\xB7  send bus", nullptr, color);
  knob(x, by, cw, kDlyTime, theme::kGreen);
  knob(x + cw, by, cw, kDlyFeedback, theme::kGreen);
  knob(x + 2 * cw, by, cw, kDlyDamp, theme::kGreen);
  ui_.Switch(Id(m.slot(kDlyPingPong), 2), Rect(x + 3 * cw + 3, by + 22, cw * 1.2f - 6, 22), "Ping-pong",
             m.slot(kDlyPingPong), theme::kGreen);
  knob(x + 4.3f * cw, by, cw, kDlyReturn, theme::kText);
  y = by + 72 + 10;

  const float half = (w - 10) * 0.5f;
  by = DrawCard(x, y, half, 72, "CHORUS  \xC2\xB7  send bus", nullptr, color);
  const float hcw = half / 3.2f;
  knob(x + 4, by, hcw, kChoRate, Rgb(0xb18cff));
  knob(x + 4 + hcw, by, hcw, kChoDepth, Rgb(0xb18cff));
  knob(x + 4 + 2 * hcw, by, hcw, kChoReturn, theme::kText);
  DrawCard(x + half + 10, y, half, 72, "OUTPUT", nullptr, color);
  knob(x + half + 14, by, hcw, kMasterWidth, theme::kText);
  knob(x + half + 14 + hcw, by, hcw, kMasterGain, theme::kText);
  DrawGainReduction(Rect(x + w - 38, by, 26, 70), limiter_db_, "LIM");
  y = by + 72 + 10;

  by = DrawCard(x, y, w, 86, "MASTER EQ  \xC2\xB7  4 bands", m.slot(kMasterEq + kEqOn), color);
  cw = w / 12.0f;
  static const char* kBandNames[kEqBands] = {"LOW SHELF", "LO-MID", "HI-MID", "HIGH SHELF"};
  for (int b = 0; b < kEqBands; ++b) {
    const float gx = x + b * 3 * cw;
    gfx_.Text(kBandNames[b], Rect(gx, by - 2, 3 * cw, 14), kBandColor[b], 9.5f, Align::kCenter, true);
    ui_.Knob(Rect(gx, by + 14, cw, 66), MasterParamInfo(kMasterEq + EqBandFreq(b)), m.slot(kMasterEq + EqBandFreq(b)),
             kBandColor[b], "Freq");
    ui_.Knob(Rect(gx + cw, by + 14, cw, 66), MasterParamInfo(kMasterEq + EqBandGain(b)), m.slot(kMasterEq + EqBandGain(b)),
             kBandColor[b], "Gain");
    ui_.Knob(Rect(gx + 2 * cw, by + 14, cw, 66), MasterParamInfo(kMasterEq + EqBandQ(b)), m.slot(kMasterEq + EqBandQ(b)),
             kBandColor[b], "Q");
  }
  y = by + 86 + 10;

  // MULTIBAND
  by = DrawCard(x, y, w, 160, "MULTIBAND  \xC2\xB7  3-band compressor", m.slot(kMasterMb + kMbOn), color);
  cw = w / 9.5f;
  knob(x, by, cw, kMasterMb + kMbLowX, theme::kYellow);
  knob(x + cw, by, cw, kMasterMb + kMbHighX, theme::kYellow);
  knob(x + 2.2f * cw, by, cw, kMasterMb + kMbAttack, theme::kOrange);
  knob(x + 3.2f * cw, by, cw, kMasterMb + kMbRelease, theme::kOrange);
  static const char* kMbNames[3] = {"LOW", "MID", "HIGH"};
  for (int b = 0; b < 3; ++b) {
    const float gx = x + b * 3 * cw;
    gfx_.Text(kMbNames[b], Rect(gx, by + 72, 3 * cw, 14), kBandColor[b == 0 ? 0 : b == 1 ? 1 : 3], 9.5f, Align::kCenter, true);
    knob(gx, by + 86, cw, kMasterMb + MbBandThresh(b), theme::kOrange);
    knob(gx + cw, by + 86, cw, kMasterMb + MbBandRatio(b), theme::kOrange);
    knob(gx + 2 * cw, by + 86, cw, kMasterMb + MbBandGain(b), theme::kOrange);
    DrawGainReduction(Rect(x + w - 104 + b * 34, by, 26, 70), mb_gr_db_[b], kMbNames[b]);
  }
  y = by + 160 + 10;

  by = DrawCard(x, y, w, 72, "BUS COMPRESSOR", m.slot(kMasterComp + kCompOn), color);
  cw = w / 7.5f;
  for (int i = 0; i < 6; ++i) knob(x + i * cw, by, cw, kMasterComp + kCompThreshold + i, theme::kOrange);
  DrawGainReduction(Rect(x + w - 38, by, 26, 70), master_meter_.gr_db);
  y = by + 72 + 10;

  // LOUDNESS (after the limiter: what actually comes out)
  by = DrawCard(x, y, w, 64, "LOUDNESS  \xC2\xB7  EBU R128 / BS.1770, after the limiter", nullptr, color);
  {
    auto value = [&](float vx, const char* label, double lufs) {
      char text[32];
      if (lufs <= -70) snprintf(text, sizeof(text), "-inf");
      else snprintf(text, sizeof(text), "%.1f", lufs);
      gfx_.Text(label, Rect(vx, by, 90, 14), theme::kDim, 10, Align::kLeft, true);
      gfx_.Text(text, Rect(vx, by + 14, 90, 26), theme::kText, 19, Align::kLeft, false, true);
      gfx_.Text("LUFS", Rect(vx, by + 40, 90, 14), theme::kFaint, 9.5f);
    };
    value(x + 6, "MOMENTARY", loudness_stats_.momentary());
    value(x + 6 + w * 0.2f, "SHORT-TERM", loudness_stats_.short_term());
    value(x + 6 + w * 0.4f, "INTEGRATED", loudness_stats_.integrated());
    // correlation: -1 .. +1
    const float cx0 = x + w * 0.6f + 6, cwid = w * 0.4f - 76;
    const double corr = loudness_stats_.correlation();
    gfx_.Text("CORRELATION", Rect(cx0, by, cwid, 14), theme::kDim, 10, Align::kLeft, true);
    const Rect bar(cx0, by + 22, cwid, 8);
    gfx_.FillRound(bar, 3, Rgb(0x0b0c10));
    const float mid = bar.cx(), pos = bar.x + bar.w * float((corr + 1.0) * 0.5);
    gfx_.Fill(Rect(std::min(mid, pos), bar.y, fabsf(pos - mid), bar.h), corr < 0 ? theme::kRed : theme::kGreen);
    gfx_.Line(mid, bar.y - 2, mid, bar.b() + 2, theme::kFaint, 1);
    char ct[16];
    snprintf(ct, sizeof(ct), "%+.2f", corr);
    gfx_.Text(ct, Rect(cx0, by + 34, cwid, 16), corr < 0 ? theme::kRed : theme::kText, 11, Align::kCenter, false, true);
    if (ui_.Button(Id(this, 116), Rect(x + w - 64, by + 18, 56, 22), "Reset", false, theme::kAccent, 11))
      loudness_stats_.Reset();
  }
  y = by + 64;
  return y;
}

// Instrument picker for the selected channel: GM families on the left, the family's eight
// programs on the right, plus "From file" (drop the override) and "Drum kit".
void ClientMainWindow::DrawInstrumentPicker(const Rect& a) {
  Gfx& g = gfx_;
  const int ch = selected_strip_;
  if (ch < 0) {
    picker_open_ = false;
    return;
  }
  static const char* kFamilies[16] = {"Piano", "Chromatic perc.", "Organ", "Guitar", "Bass", "Strings",
                                      "Ensemble", "Brass", "Reed", "Pipe", "Synth lead", "Synth pad",
                                      "Synth effects", "Ethnic", "Percussive", "Sound effects"};
  const float row = 21, head = 40;
  const Rect box(a.x, a.y, a.w, head + 16 * row + 10);
  const Color color = theme::Channel(ch);
  const int current = midi_synth_->channel_override(ch);
  const int sounding = midi_synth_->channel_program(ch);

  // Click outside closes.
  if (ui_.in.pressed && !box.Contains(ui_.in.mx, ui_.in.my)) {
    ui_.in.pressed = false;
    picker_open_ = false;
    return;
  }
  g.FillRound(Rect(box.x + 3, box.y + 5, box.w, box.h), 8, Rgb(0x000000, 0.45f));
  g.FillRound(box, 8, theme::kPanel2);
  g.StrokeRound(box, 8, theme::kLine);
  char title[64];
  snprintf(title, sizeof(title), "Instrument for channel %d", ch + 1);
  g.Text(title, Rect(box.x + 14, box.y + 8, box.w - 200, 24), theme::kText, 13, Align::kLeft, true);
  if (ui_.Button(Id(this, 60), Rect(box.r() - 186, box.y + 9, 88, 22), "From file", current < 0, color, 11)) {
    midi_synth_->set_channel_override(ch, -1);
    picker_open_ = false;
  }
  if (ui_.Button(Id(this, 61), Rect(box.r() - 94, box.y + 9, 82, 22), "Drum kit", current == 128, color, 11)) {
    midi_synth_->set_channel_override(ch, 128);
    picker_open_ = false;
  }

  const float fam_w = 150;
  for (int f = 0; f < 16; ++f) {
    const Rect fr(box.x + 8, box.y + head + f * row, fam_w, row - 2);
    const bool on = f == picker_family_;
    const bool has_current = sounding < 128 && sounding / 8 == f;
    if (ui_.Hover(fr)) picker_family_ = f;  // hovering a family shows its programs
    ui_.Clicked(Id(this, 100 + f), fr);
    g.FillRound(fr, 4, on ? theme::kPanel3 : Rgb(0, 0));
    g.Text(kFamilies[f], fr.Inset(10, 0), has_current ? color : on ? theme::kText : theme::kDim, 11.5f, Align::kLeft,
           has_current);
  }
  const float px = box.x + fam_w + 18, pw = box.r() - 10 - px;
  g.Line(px - 6, box.y + head, px - 6, box.b() - 10, theme::kLine);
  for (int i = 0; i < 8; ++i) {
    const int program = picker_family_ * 8 + i;
    const Rect pr(px, box.y + head + i * row * 2, pw, row * 2 - 4);
    char label[64];
    snprintf(label, sizeof(label), "%3d   %s", program + 1, ProgramName(program));
    const bool on = program == sounding;
    if (ui_.Clicked(Id(this, 200 + program), pr)) {
      midi_synth_->set_channel_override(ch, program);
      picker_open_ = false;
    }
    const bool over = ui_.Hover(pr);
    g.FillRound(pr, 5, on ? WithAlpha(color, 0.85f) : over ? theme::kPanel3 : theme::kPanel);
    g.Text(label, pr.Inset(12, 0), on ? Rgb(0x0d0f13) : theme::kText, 12, Align::kLeft, on);
  }
}

// Frequency response of the selected strip (filters + EQ) or the master EQ, over a live
// spectrum of the same point in the signal path. Band nodes drag (freq/gain), wheel = Q.
void ClientMainWindow::DrawResponseGraph(const Rect& r, int strip) {
  Gfx& g = gfx_;
  auto& mixer = midi_synth_->mixer;
  const double fs = mixer.sample_rate() ? mixer.sample_rate() : 48000;
  const bool is_master = strip < 0;
  const int eq_base = is_master ? kMasterEq : kStripEq;
  auto slot = [&](int id) { return is_master ? mixer.master().slot(id) : mixer.strip(strip).slot(id); };
  auto info = [&](int id) -> const ParamInfo& { return is_master ? MasterParamInfo(id) : StripParamInfo(id); };
  auto get = [&](int id) { return slot(id)->load(std::memory_order_relaxed); };
  const Color accent = is_master ? theme::kText : theme::Channel(strip);

  g.FillRound(r, 6, Rgb(0x0d0f13));
  const Rect plot(r.x + 30, r.y + 22, r.w - 40, r.h - 40);
  const float kMinF = 20, kMaxF = 20000, kRange = 24;
  auto fx = [&](double f) { return plot.x + float(log(f / kMinF) / log(kMaxF / kMinF)) * plot.w; };
  auto xf = [&](float px) { return kMinF * pow(kMaxF / kMinF, std::clamp((px - plot.x) / plot.w, 0.0f, 1.0f)); };
  auto gy = [&](double db) { return plot.cy() - float(db / kRange) * plot.h * 0.5f; };

  // grid
  static const float kFreqs[] = {20, 50, 100, 200, 500, 1000, 2000, 5000, 10000, 20000};
  static const char* kFreqLabels[] = {"20", "50", "100", "200", "500", "1k", "2k", "5k", "10k", "20k"};
  for (int i = 0; i < 10; ++i) {
    const float gx = fx(kFreqs[i]);
    g.Line(gx, plot.y, gx, plot.b(), Rgb(0x1f232b), 1);
    g.Text(kFreqLabels[i], Rect(gx - 20, plot.b() + 2, 40, 14), theme::kFaint, 9.5f, Align::kCenter);
  }
  for (int db = -18; db <= 18; db += 6) {
    const float yy = gy(db);
    g.Line(plot.x, yy, plot.r(), yy, db == 0 ? Rgb(0x2c323d) : Rgb(0x1a1e25), 1);
    char t[8];
    snprintf(t, sizeof(t), "%+d", db);
    g.Text(db == 0 ? "0" : t, Rect(r.x + 2, yy - 7, 26, 14), theme::kFaint, 9, Align::kRight);
  }

  // live spectrum (-90..0 dBFS over the plot height)
  std::vector<D2D1_POINT_2F> pts;
  if (!spectrum_db_.empty()) {
    const int bins = int(spectrum_db_.size());
    for (float px = plot.x; px <= plot.r(); px += 2) {
      const double f0 = xf(px), f1 = xf(px + 2);
      int b0 = int(f0 / fs * (bins - 1) * 2), b1 = int(f1 / fs * (bins - 1) * 2);
      b0 = std::clamp(b0, 1, bins - 1);
      b1 = std::clamp(std::max(b1, b0), 1, bins - 1);
      float db = -140;
      for (int b = b0; b <= b1; ++b) db = std::max(db, spectrum_db_[b]);
      // +3 dB/octave tilt from 1 kHz so a balanced mix looks roughly flat
      db += float(3.0 * log2(std::max(20.0, (f0 + f1) * 0.5) / 1000.0));
      const float yy = plot.b() - plot.h * std::clamp((db + 90.0f) / 90.0f, 0.0f, 1.0f);
      pts.push_back(D2D1::Point2F(px, yy));
    }
    g.FillUnder(pts.data(), int(pts.size()), plot.b(), WithAlpha(accent, 0.10f));
    g.Polyline(pts.data(), int(pts.size()), WithAlpha(accent, 0.35f), 1.0f);
  }

  // response curve
  float eq[kEqParamCount];
  for (int i = 0; i < kEqParamCount; ++i) eq[i] = get(eq_base + i);
  const bool eq_on = eq[kEqOn] >= 0.5f;
  const bool hpf_on = !is_master && get(kStripHpfOn) >= 0.5f, lpf_on = !is_master && get(kStripLpfOn) >= 0.5f;
  BiquadCoefs hpf, lpf;
  if (hpf_on) hpf = DesignBiquad(BiquadType::kHighPass, fs, get(kStripHpfFreq), 0.7071, 0);
  if (lpf_on) lpf = DesignBiquad(BiquadType::kLowPass, fs, get(kStripLpfFreq), get(kStripLpfQ), 0);
  const int stages = !is_master && get(kStripHpfSteep) >= 0.5f ? 2 : 1;
  pts.clear();
  for (float px = plot.x; px <= plot.r() + 0.5f; px += 2) {
    const double f = xf(px);
    double db = eq_on ? EqResponseDb(eq, fs, f) : 0.0;
    if (hpf_on) db += stages * BiquadMagnitudeDb(hpf, fs, f);
    if (lpf_on) db += BiquadMagnitudeDb(lpf, fs, f);
    pts.push_back(D2D1::Point2F(px, std::clamp(gy(db), plot.y - 2, plot.b() + 2)));
  }
  g.PushClip(Rect(plot.x, plot.y - 2, plot.w, plot.h + 4));
  const bool any = eq_on || hpf_on || lpf_on;
  g.FillUnder(pts.data(), int(pts.size()), gy(0), WithAlpha(accent, any ? 0.16f : 0.05f));
  g.Polyline(pts.data(), int(pts.size()), any ? accent : theme::kFaint, 2.0f);
  g.PopClip();

  // band nodes
  const char* kNames[kEqBands] = {"Low", "Lo-Mid", "Hi-Mid", "High"};
  for (int b = 0; b < kEqBands; ++b) {
    const int fid = eq_base + EqBandFreq(b), gid = eq_base + EqBandGain(b), qid = eq_base + EqBandQ(b);
    const float nx = fx(get(fid)), ny = gy(get(gid));
    const WidgetId id = Id(slot(fid), 77);
    const Rect hit(nx - 9, ny - 9, 18, 18);
    if (ui_.Press(id, hit)) {
      if (ui_.in.dbl) slot(gid)->store(0.0f);
      if (!eq_on) slot(eq_base + kEqOn)->store(1.0f);
    }
    if (ui_.Dragging(id) && !ui_.in.dbl) {
      const auto& fi = info(fid);
      const auto& gi = info(gid);
      slot(fid)->store(std::clamp(float(xf(ui_.in.mx)), fi.min, fi.max));
      const float gain = -(ui_.in.my - plot.cy()) / (plot.h * 0.5f) * kRange;
      slot(gid)->store(std::clamp(roundf(gain * 10) / 10, gi.min, gi.max));
    }
    if (ui_.knob_wheel && ui_.Hover(hit) && ui_.active == 0) {
      const float wheel = ui_.TakeWheel(hit);
      if (wheel != 0) {
        const auto& qi = info(qid);
        slot(qid)->store(std::clamp(get(qid) * powf(1.15f, wheel), qi.min, qi.max));
      }
    }
    const bool engaged = ui_.hot == id || ui_.active == id;
    const Color c = kBandColor[b];
    g.Circle(nx, ny, engaged ? 7.5f : 6.0f, eq_on ? c : WithAlpha(c, 0.35f), true);
    g.Circle(nx, ny, engaged ? 7.5f : 6.0f, Rgb(0x0d0f13), false, 1.5f);
    char t[8];
    snprintf(t, sizeof(t), "%d", b + 1);
    g.Text(t, Rect(nx - 6, ny - 6, 12, 12), Rgb(0x0d0f13), 8.5f, Align::kCenter, true);
    if (engaged) {
      char f[24], gs[24], tip[96];
      FormatParam(info(fid), get(fid), f, sizeof(f));
      FormatParam(info(gid), get(gid), gs, sizeof(gs));
      snprintf(tip, sizeof(tip), "%s  %s  %s  Q %.2f   (wheel = Q)", kNames[b], f, gs, get(qid));
      ui_.Tooltip(tip, nx, ny - 6);
    }
  }

  // high-pass / low-pass handles (channel strips)
  if (!is_master) {
    const int ids[2] = {kStripHpfFreq, kStripLpfFreq};
    const int ons[2] = {kStripHpfOn, kStripLpfOn};
    const char* names[2] = {"HPF", "LPF"};
    for (int k = 0; k < 2; ++k) {
      const float hx = fx(get(ids[k])), hy = gy(-3);
      const WidgetId id = Id(slot(ids[k]), 78);
      const Rect hit(hx - 9, hy - 9, 18, 18);
      if (ui_.Press(id, hit) && get(ons[k]) < 0.5f) slot(ons[k])->store(1.0f);
      if (ui_.Dragging(id)) {
        const auto& fi = info(ids[k]);
        slot(ids[k])->store(std::clamp(float(xf(ui_.in.mx)), fi.min, fi.max));
      }
      const bool on = get(ons[k]) >= 0.5f;
      const bool engaged = ui_.hot == id || ui_.active == id;
      const Rect box(hx - 5.5f, hy - 5.5f, 11, 11);
      g.FillRound(box, 2, on ? accent : WithAlpha(accent, 0.3f));
      g.StrokeRound(box, 2, Rgb(0x0d0f13), 1.5f);
      if (engaged) {
        char f[24], tip[64];
        FormatParam(info(ids[k]), get(ids[k]), f, sizeof(f));
        snprintf(tip, sizeof(tip), "%s  %s", names[k], f);
        ui_.Tooltip(tip, hx, hy - 6);
      }
    }
  }

  char label[64];
  if (is_master) snprintf(label, sizeof(label), "MASTER OUT  \xC2\xB7  spectrum + EQ");
  else snprintf(label, sizeof(label), "CH %d POST-FADER  \xC2\xB7  spectrum + filters + EQ", strip + 1);
  g.Text(label, Rect(r.x + 12, r.y + 3, r.w - 24, 16), theme::kDim, 10, Align::kLeft, true);
}

}
