// Automation lanes, Touch recording, the parameter context menu and MIDI learn.
#include "global.h"

namespace demo {

using namespace dsp::audio::synth;
using namespace dsp::audio::synth::effects;

static const int kBitsPerStrip = 128;

bool ClientMainWindow::SlotToParam(const std::atomic<float>* slot, int* strip, int* param) const {
  if (!slot || !midi_synth_) return false;
  auto& mixer = midi_synth_->mixer;
  for (int s = 0; s < Mixer::kStrips; ++s) {
    const std::atomic<float>* base = mixer.strip(s).slot(0);
    if (slot >= base && slot < base + kStripParamCount) {
      *strip = s;
      *param = int(slot - base);
      return true;
    }
  }
  const std::atomic<float>* base = mixer.master().slot(0);
  if (slot >= base && slot < base + kMasterParamCount) {
    *strip = -1;
    *param = int(slot - base);
    return true;
  }
  return false;
}

// "Ch 2 Strings · eq lomid gain", "Master · output"
std::string ClientMainWindow::ParamLabel(int strip, int param) const {
  std::string key = strip < 0 ? MasterParamKey(param) : StripParamKey(param);
  for (auto& c : key)
    if (c == '.' || c == '_') c = ' ';
  if (strip < 0) return "Master \xC2\xB7 " + key;
  std::string who = "Ch " + std::to_string(strip + 1);
  if (!strip_names_[strip].empty()) who += " " + strip_names_[strip];
  return who + " \xC2\xB7 " + key;
}

int ClientMainWindow::FindLane(int strip, int param) const {
  const auto& lanes = midi_synth_->song().automation;
  for (int i = 0; i < int(lanes.size()); ++i)
    if (lanes[i].strip == strip && lanes[i].param == param) return i;
  return -1;
}

// A new lane starts with one point holding the current value, so nothing changes until it is
// edited or recorded over.
void ClientMainWindow::AddAutomationLane(int strip, int param) {
  if (FindLane(strip, param) >= 0) return;
  SongInfo song = midi_synth_->song();
  if (!song.loaded) MakeEmptySong(song);
  AutomationLane lane;
  lane.strip = strip;
  lane.param = param;
  const float value = strip < 0 ? midi_synth_->mixer.master().get(param) : midi_synth_->mixer.strip(strip).get(param);
  lane.points.push_back({0, value, 0});
  song.automation.push_back(lane);
  CommitSong(std::move(song));
  ShowMessage("Automation lane for " + ParamLabel(strip, param) +
              ": double-click the lane to add points, or turn on Touch and move the control while playing");
}

void ClientMainWindow::RemoveAutomationLane(int strip, int param) {
  const int index = FindLane(strip, param);
  if (index < 0) return;
  SongInfo song = midi_synth_->song();
  song.automation.erase(song.automation.begin() + index);
  CommitSong(std::move(song));
  ShowMessage("Removed automation for " + ParamLabel(strip, param) + "  (Ctrl+Z to undo)");
}

void ClientMainWindow::RefreshMappedBits() {
  mapped_bits_.assign(size_t(Mixer::kStrips + 1) * kBitsPerStrip, 0);
  for (int ch = 0; ch < 16; ++ch)
    for (int cc = 0; cc < 128; ++cc) {
      int strip, param;
      if (midi_synth_->MappedTarget(ch, cc, &strip, &param) && param < kBitsPerStrip)
        mapped_bits_[size_t(strip + 1) * kBitsPerStrip + param] = 1;
    }
}

// ---------------------------------------------------------------------------------------------
// Touch: while playing with Touch on, moving a control records its value into its lane (made if
// needed), replacing what the lane had over the touched stretch. Any control being dragged is
// left alone by automation playback, Touch or not.
// ---------------------------------------------------------------------------------------------
void ClientMainWindow::UpdateTouch() {
  int strip = 0, param = -1;
  const bool holding = SlotToParam(ui_.active_slot, &strip, &param);
  midi_synth_->set_touched_param(holding ? strip : 0, holding ? param : -1);
  const bool playing = synth_player_->state() == RealtimePlayer::kStatePlaying && !midi_synth_->counting_in();
  if (!(touch_mode_ && playing && holding)) {
    if (touching_) EndTouch();
    return;
  }
  const double ms = PlayheadMs();
  if (touching_ && (strip != touch_strip_ || param != touch_param_ ||
                    (!touch_points_.empty() && ms < touch_points_.back().first - 50))) {
    EndTouch();  // another control, or the loop jumped back
  }
  if (!touching_) {
    touching_ = true;
    touch_strip_ = strip;
    touch_param_ = param;
    touch_points_.clear();
  }
  const float value = ui_.active_slot->load();
  if (touch_points_.empty() || ms - touch_points_.back().first >= 30 ||
      (value != touch_points_.back().second && ms - touch_points_.back().first >= 10))
    touch_points_.push_back({ms, value});
}

void ClientMainWindow::EndTouch() {
  touching_ = false;
  if (touch_points_.empty()) return;
  SongInfo song = midi_synth_->song();
  int index = -1;
  for (int i = 0; i < int(song.automation.size()); ++i)
    if (song.automation[i].strip == touch_strip_ && song.automation[i].param == touch_param_) index = i;
  if (index < 0) {
    song.automation.push_back({touch_strip_, touch_param_, {}});
    index = int(song.automation.size()) - 1;
  }
  auto& pts = song.automation[index].points;
  const double t0 = touch_points_.front().first, t1 = touch_points_.back().first;
  pts.erase(std::remove_if(pts.begin(), pts.end(), [&](const AutomationPoint& p) { return p.ms >= t0 && p.ms <= t1; }),
            pts.end());
  for (size_t i = 0; i < touch_points_.size(); ++i) {
    // keep the shape, drop points on a straight run
    if (i > 0 && i + 1 < touch_points_.size()) {
      const auto& a = touch_points_[i - 1];
      const auto& b = touch_points_[i];
      const auto& c = touch_points_[i + 1];
      const double t = (b.first - a.first) / std::max(1e-9, c.first - a.first);
      if (fabs(a.second + (c.second - a.second) * t - b.second) < 1e-4) continue;
    }
    pts.push_back({song.MsToTick(touch_points_[i].first), touch_points_[i].second, 0});
  }
  touch_points_.clear();
  CommitSong(std::move(song));
  ShowMessage("Recorded automation for " + ParamLabel(touch_strip_, touch_param_) + "  (Ctrl+Z to undo)");
}

// ---------------------------------------------------------------------------------------------
// Right-click menu on knobs and faders
// ---------------------------------------------------------------------------------------------
void ClientMainWindow::DrawParamMenu(const Rect& a) {
  Gfx& g = gfx_;
  const int strip = menu_strip_, param = menu_param_;
  const bool has_lane = FindLane(strip, param) >= 0;
  const bool learning = midi_synth_->midi_learning(strip, param);
  const int mapping = midi_synth_->MidiMappingFor(strip, param);
  char mapping_label[64];
  snprintf(mapping_label, sizeof(mapping_label), "Remove MIDI mapping (CC %d, ch %d)", mapping & 0xFF, (mapping >> 8) + 1);
  const char* items[4] = {has_lane ? "Remove automation lane" : "Add automation lane",
                          learning ? "Cancel MIDI learn" : "MIDI learn", mapping >= 0 ? mapping_label : nullptr,
                          "Reset to default"};
  const float row = 30, w = 270;
  int count = 0;
  for (auto* it : items) count += it != nullptr;
  Rect box(a.x, a.y, w, 30 + count * row + 8);
  if (box.r() > g.width() - 8) box.x = g.width() - 8 - w;
  if (box.b() > g.height() - 8) box.y = g.height() - 8 - box.h;
  if (ui_.in.pressed && !box.Contains(ui_.in.mx, ui_.in.my)) {
    ui_.in.pressed = false;
    param_menu_open_ = false;
    return;
  }
  g.FillRound(Rect(box.x + 3, box.y + 5, box.w, box.h), 6, Rgb(0x000000, 0.45f));
  g.FillRound(box, 6, theme::kPanel2);
  g.StrokeRound(box, 6, theme::kLine);
  g.Text(ParamLabel(strip, param).c_str(), Rect(box.x + 12, box.y + 4, box.w - 24, 24), theme::kDim, 11, Align::kLeft, true);
  float y = box.y + 30;
  for (int i = 0; i < 4; ++i) {
    if (!items[i]) continue;
    const Rect item(box.x + 6, y, box.w - 12, row - 2);
    if (ui_.Hover(item)) g.FillRound(item, 4, theme::kPanel3);
    g.Text(items[i], item.Inset(10, 0), theme::kText, 12);
    if (ui_.Clicked(Id(this, 130 + i), item)) {
      param_menu_open_ = false;
      if (i == 0) {
        if (has_lane) RemoveAutomationLane(strip, param);
        else AddAutomationLane(strip, param);
      } else if (i == 1) {
        if (learning) {
          midi_synth_->CancelMidiLearn();
          ShowMessage("MIDI learn cancelled");
        } else {
          midi_synth_->StartMidiLearn(strip, param);
          ShowMessage("MIDI learn: move a knob or fader on your MIDI controller (" + ParamLabel(strip, param) + ")");
        }
      } else if (i == 2) {
        midi_synth_->ClearMidiMapping(strip, param);
        ShowMessage("Removed the MIDI mapping of " + ParamLabel(strip, param));
      } else {
        const auto& info = strip < 0 ? MasterParamInfo(param) : StripParamInfo(param);
        if (strip < 0) midi_synth_->mixer.master().set(param, info.def);
        else midi_synth_->mixer.strip(strip).set(param, info.def);
      }
    }
    y += row;
  }
}

// ---------------------------------------------------------------------------------------------
// On-screen keyboard: 88 keys, lit with the colour of the channel playing each note (file or live
// input); click to play (channel 1, the PC-keys program; lower on the key = louder). The range
// the PC keys reach is underlined.
// ---------------------------------------------------------------------------------------------
void ClientMainWindow::DrawKeyboard(const Rect& r) {
  Gfx& g = gfx_;
  g.Fill(r, theme::kPanel);
  g.Line(r.x, r.y + 0.5f, r.r(), r.y + 0.5f, theme::kLine);
  const int lo = 21, hi = 108;  // A0 .. C8
  auto black = [](int n) { const int k = n % 12; return k == 1 || k == 3 || k == 6 || k == 8 || k == 10; };
  int whites = 0;
  for (int n = lo; n <= hi; ++n) whites += !black(n);
  const Rect keys(r.x + 8, r.y + 5, r.w - 16, r.h - 9);
  const float ww = keys.w / whites;
  auto lit_color = [&](int note, Color base) {
    const uint16_t mask = midi_synth_->channels_holding(note);
    if (!mask) return base;
    int ch = 0;
    while (!(mask & (1 << ch))) ++ch;
    return Mix(base, theme::Channel(ch), 0.85f);
  };
  float x = keys.x;
  float white_x[128] = {};
  for (int n = lo; n <= hi; ++n) {
    if (black(n)) continue;
    white_x[n] = x;
    const Rect k(x, keys.y, ww - 1, keys.h);
    g.FillRound(k, 2, n == kb_note_ ? Rgb(0x9fb4d0) : lit_color(n, Rgb(0xd9dce2)));
    if (n % 12 == 0 && ww > 9) {
      char label[8];
      snprintf(label, sizeof(label), "C%d", n / 12 - 1);
      g.Text(label, Rect(k.x, k.b() - 13, k.w, 12), Rgb(0x5a6070), 8.5f, Align::kCenter);
    }
    x += ww;
  }
  const float bw = ww * 0.62f, bh = keys.h * 0.6f;
  for (int n = lo; n <= hi; ++n) {
    if (!black(n)) continue;
    const float bx = white_x[n - 1] + ww - bw * 0.5f;
    g.FillRound(Rect(bx, keys.y, bw, bh), 2, n == kb_note_ ? Rgb(0x6f86a8) : lit_color(n, Rgb(0x23262d)));
  }
  // PC-keys range
  const int pc_lo = std::max(lo, octave_base_), pc_hi = std::min(hi, octave_base_ + 28);
  if (pc_lo <= pc_hi && !black(pc_lo)) {
    float x1 = white_x[pc_hi] ? white_x[pc_hi] + ww : white_x[pc_hi - 1] + ww;
    g.Fill(Rect(white_x[pc_lo], keys.b() + 1, x1 - white_x[pc_lo], 2), WithAlpha(theme::kAccent, 0.8f));
  }

  // mouse: black keys first (they sit on top)
  auto note_at = [&](float mx, float my, float* depth) -> int {
    if (my < keys.y || my > keys.b() || mx < keys.x || mx > keys.r()) return -1;
    if (my < keys.y + bh)
      for (int n = lo; n <= hi; ++n)
        if (black(n)) {
          const float bx = white_x[n - 1] + ww - bw * 0.5f;
          if (mx >= bx && mx < bx + bw) { *depth = (my - keys.y) / bh; return n; }
        }
    for (int n = lo; n <= hi; ++n)
      if (!black(n) && mx >= white_x[n] && mx < white_x[n] + ww) { *depth = (my - keys.y) / keys.h; return n; }
    return -1;
  };
  const WidgetId id = Id(this, 140);
  float depth = 0.5f;
  if (ui_.Press(id, keys)) {
    const int n = note_at(ui_.in.mx, ui_.in.my, &depth);
    if (n >= 0) {
      kb_note_ = n;
      SendLive(0x90, uint8_t(n), uint8_t(std::clamp(int(40 + depth * 87), 1, 127)));
    }
  }
  if (ui_.active == id) {
    const int n = note_at(ui_.in.mx, ui_.in.my, &depth);
    if (!ui_.in.down || n != kb_note_) {  // released, or slid onto another key
      if (kb_note_ >= 0) SendLive(0x80, uint8_t(kb_note_), 64);
      kb_note_ = -1;
      if (ui_.in.down && n >= 0) {
        kb_note_ = n;
        SendLive(0x90, uint8_t(n), uint8_t(std::clamp(int(40 + depth * 87), 1, 127)));
      }
      if (!ui_.in.down) ui_.active = 0;
    }
  }
}

bool ClientMainWindow::StripUsed(int ch) const {
  return !strip_names_[ch].empty() || selected_strip_ == ch || midi_synth_->channel_override(ch) >= 0 ||
         strip_meter_[ch].hold[0] > -80.0f || strip_meter_[ch].hold[1] > -80.0f;
}

// ---------------------------------------------------------------------------------------------
// One automation lane row in the track view
// ---------------------------------------------------------------------------------------------
void ClientMainWindow::DrawAutomationRow(const Rect& head, const Rect& lane, int index, double view_start_ms,
                                         double ms_per_px) {
  Gfx& g = gfx_;
  const auto& lanes = midi_synth_->song().automation;
  if (index < 0 || index >= int(lanes.size())) return;
  const AutomationLane& al = lanes[index];
  const auto& info = al.strip < 0 ? MasterParamInfo(al.param) : StripParamInfo(al.param);
  const Color color = theme::kYellow;

  g.Fill(head, Mix(theme::kPanel, theme::kYellow, 0.06f));
  g.Fill(Rect(head.x, head.y + 1, 4, head.h - 2), color);
  g.Text(ParamLabel(al.strip, al.param).c_str(), Rect(head.x + 14, head.y + 4, head.w - 50, 18), theme::kText, 11.5f,
         Align::kLeft, true);
  char current[32];
  FormatParam(info, al.strip < 0 ? midi_synth_->mixer.master().get(al.param) : midi_synth_->mixer.strip(al.strip).get(al.param),
              current, sizeof(current));
  char detail[96];
  snprintf(detail, sizeof(detail), "automation  \xC2\xB7  %zu points  \xC2\xB7  now %s", al.points.size(), current);
  if (head.h >= 36) g.Text(detail, Rect(head.x + 14, head.y + 21, head.w - 22, 14), theme::kDim, 10);
  const Rect del(head.r() - 30, head.y + 5, 22, 18);
  if (ui_.Button(Id(&al, 1), del, "\xC3\x97", false, theme::kRed, 11)) {
    RemoveAutomationLane(al.strip, al.param);
    return;
  }

  g.Fill(lane, WithAlpha(theme::kYellow, 0.03f));
  const float pad = 5;
  auto x_of = [&](double ms) { return lane.x + float((ms - view_start_ms) / ms_per_px); };
  auto y_of = [&](float v) { return lane.b() - pad - (lane.h - 2 * pad) * ParamToNormalized(info, v); };
  auto value_at_y = [&](float py) {
    return ParamFromNormalized(info, std::clamp((lane.b() - pad - py) / (lane.h - 2 * pad), 0.0f, 1.0f));
  };
  const auto& song = midi_synth_->song();
  auto ms_at = [&](float px) { return std::max(0.0, view_start_ms + (px - lane.x) * ms_per_px); };

  // points, with the one being dragged at the mouse
  std::vector<std::pair<double, float>> pts;
  for (size_t i = 0; i < al.points.size(); ++i) {
    if (lane_drag_ == index && int(i) == lane_drag_point_)
      pts.push_back({ms_at(ui_.in.mx), value_at_y(ui_.in.my)});
    else
      pts.push_back({al.points[i].ms, al.points[i].value});
  }
  std::sort(pts.begin(), pts.end());
  g.PushClip(lane);
  g.Line(lane.x, y_of(info.def), lane.r(), y_of(info.def), Mix(theme::kLine, theme::kBg, 0.4f), 1);
  if (!pts.empty()) {
    std::vector<D2D1_POINT_2F> line;
    line.push_back(D2D1::Point2F(lane.x - 2, y_of(pts.front().second)));
    for (auto& p : pts) line.push_back(D2D1::Point2F(x_of(p.first), y_of(p.second)));
    line.push_back(D2D1::Point2F(lane.r() + 2, y_of(pts.back().second)));
    g.FillUnder(line.data(), int(line.size()), lane.b(), WithAlpha(color, 0.10f));
    g.Polyline(line.data(), int(line.size()), color, 1.8f);
  }
  int hit = -1;
  for (size_t i = 0; i < al.points.size(); ++i) {
    const float px = x_of(al.points[i].ms), py = y_of(al.points[i].value);
    const bool over = fabsf(ui_.in.mx - px) <= 6 && fabsf(ui_.in.my - py) <= 6 && ui_.Hover(lane);
    if (over) hit = int(i);
    if (!(lane_drag_ == index && int(i) == lane_drag_point_))
      g.Circle(px, py, over ? 5.0f : 3.5f, color, true);
  }
  g.PopClip();

  // editing: drag a point; double-click adds one (or deletes the one under the mouse)
  auto commit = [&](const std::function<void(AutomationLane&)>& fn) {
    SongInfo s = song;
    fn(s.automation[index]);
    CommitSong(std::move(s));
  };
  if (ui_.Hover(lane) && ui_.in.pressed && ui_.in.dbl && ui_.active == 0) {
    ui_.in.pressed = false;
    if (hit >= 0) {
      commit([hit](AutomationLane& l) { l.points.erase(l.points.begin() + hit); });
    } else {
      const uint64_t tick = song.MsToTick(ms_at(ui_.in.mx));
      const float v = value_at_y(ui_.in.my);
      commit([&](AutomationLane& l) { l.points.push_back({tick, v, 0}); });
    }
    return;
  }
  const WidgetId id = Id(&al, 2);
  if (hit >= 0 && ui_.Press(id, lane)) {
    lane_drag_ = index;
    lane_drag_point_ = hit;
  }
  if (ui_.active == id && lane_drag_ == index) {
    char tip[64], value[32];
    FormatParam(info, value_at_y(ui_.in.my), value, sizeof(value));
    snprintf(tip, sizeof(tip), "%s", value);
    ui_.Tooltip(tip, ui_.in.mx, lane.y);
    if (!ui_.in.down) {
      const int point = lane_drag_point_;
      const uint64_t tick = song.MsToTick(ms_at(ui_.in.mx));
      const float v = value_at_y(ui_.in.my);
      lane_drag_ = lane_drag_point_ = -1;
      ui_.active = 0;
      commit([&](AutomationLane& l) {
        if (point < int(l.points.size())) l.points[point] = {tick, v, 0};
      });
    }
  }
}

}
