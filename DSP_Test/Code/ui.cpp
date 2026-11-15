#include "global.h"

namespace demo {

using dsp::audio::synth::effects::FormatParam;
using dsp::audio::synth::effects::ParamFromNormalized;
using dsp::audio::synth::effects::ParamInfo;
using dsp::audio::synth::effects::ParamToNormalized;
using dsp::audio::synth::effects::ParamUnit;

namespace theme {
Color Channel(int index) {
  static const unsigned kColors[17] = {
    0x4ea1ff, 0x3ddc84, 0xffb84a, 0xff6b8a, 0xb18cff, 0x40d9d0, 0xff8f5a, 0xa6d84a,
    0xf279d8, 0x9aa7ff, 0x5ecbff, 0xe6d24a, 0xff7a6b, 0x6be6a5, 0xc79cff, 0x7fc8a9, 0xe7e9ee};
  return Rgb(kColors[index < 0 || index > 16 ? 16 : index]);
}
}

void Ui::BeginFrame() {
  hot = 0;
  tip_.clear();
}

void Ui::EndFrame() {
  if (!tip_.empty()) {
    const float w = g.TextWidth(tip_.c_str(), 12.0f) + 14;
    float x = tip_x_ - w * 0.5f, y = tip_y_ - 26;
    if (x < 4) x = 4;
    if (x + w > g.width() - 4) x = g.width() - 4 - w;
    if (y < 4) y = tip_y_ + 8;
    Rect r(x, y, w, 20);
    g.FillRound(r, 4, Rgb(0x000000, 0.85f));
    g.StrokeRound(r, 4, theme::kLine);
    g.Text(tip_.c_str(), r, theme::kText, 12.0f, Align::kCenter);
  }
  in.pressed = in.released = in.dbl = in.rpressed = false;
  in.wheel = 0;
  if (!in.down) active = 0;  // a release we never saw (e.g. focus lost)
}

bool Ui::Press(WidgetId id, const Rect& r) {
  const bool over = Over(id, r);
  if (over) hot = id;
  if (over && in.pressed && active == 0) {
    active = id;
    in.pressed = false;  // consumed
    return true;
  }
  return false;
}

bool Ui::Clicked(WidgetId id, const Rect& r) {
  Press(id, r);
  if (active == id && in.released) {
    active = 0;
    in.released = false;
    return Hover(r);
  }
  return false;
}

float Ui::TakeWheel(const Rect& r) {
  if (in.wheel == 0 || !Hover(r) || (active != 0 && in.down)) return 0;
  const float w = in.wheel;
  in.wheel = 0;
  return w;
}

void Ui::Tooltip(const char* text, float x, float y) {
  tip_ = text;
  tip_x_ = x;
  tip_y_ = y;
}

bool Ui::Button(WidgetId id, const Rect& r, const char* label, bool on, Color on_color, float text_size) {
  const bool clicked = Clicked(id, r);
  const bool over = Hover(r) && (active == 0 || active == id);
  Color bg = on ? on_color : theme::kPanel3;
  if (!on && over) bg = Mix(bg, theme::kText, 0.08f);
  if (active == id) bg = Mix(bg, Rgb(0), 0.2f);
  g.FillRound(r, 4, bg);
  if (!on) g.StrokeRound(r, 4, Mix(theme::kLine, theme::kText, over ? 0.15f : 0.0f));
  g.Text(label, r, on ? Rgb(0x0d0f13) : theme::kText, text_size, Align::kCenter, on);
  return clicked;
}

bool Ui::Switch(WidgetId id, const Rect& r, const char* label, std::atomic<float>* v, Color on_color) {
  const bool on = v->load(std::memory_order_relaxed) >= 0.5f;
  if (Button(id, r, label, on, on_color, 11.0f)) {
    v->store(on ? 0.0f : 1.0f, std::memory_order_relaxed);
    return true;
  }
  return false;
}

bool Ui::Power(WidgetId id, float cx, float cy, std::atomic<float>* v, Color on_color) {
  const Rect r(cx - 9, cy - 9, 18, 18);
  const bool on = v->load(std::memory_order_relaxed) >= 0.5f;
  const bool clicked = Clicked(id, r);
  const bool over = Hover(r);
  g.Circle(cx, cy, 8, on ? on_color : (over ? theme::kPanel3 : theme::kPanel2), true);
  g.Circle(cx, cy, 8, on ? on_color : theme::kLine, false, 1.0f);
  const Color glyph = on ? Rgb(0x0d0f13) : theme::kDim;
  g.Arc(cx, cy, 4.2f, 0.6f, 6.283f - 0.6f, glyph, 1.6f);
  g.Line(cx, cy - 5.5f, cx, cy - 1.0f, glyph, 1.6f);
  if (clicked) {
    v->store(on ? 0.0f : 1.0f, std::memory_order_relaxed);
    return true;
  }
  return false;
}

bool Ui::DragValue(WidgetId id, const Rect& r, const ParamInfo& info, std::atomic<float>* v, float pixels_per_range) {
  const float value = v->load(std::memory_order_relaxed);
  bool changed = false;
  if (Over(id, r)) hot = id;
  if (Over(id, r) && in.pressed && active == 0) {
    active = id;
    in.pressed = false;
    if (in.dbl) {
      v->store(info.def, std::memory_order_relaxed);
      changed = true;
    }
    drag_y0_ = in.my;
    drag_t0_ = ParamToNormalized(info, in.dbl ? info.def : value);
  }
  if (active == id && in.down && !in.dbl) {
    const float t = drag_t0_ + (drag_y0_ - in.my) / (pixels_per_range * (in.shift ? 5.0f : 1.0f));
    const float nv = ParamFromNormalized(info, t);
    if (nv != value) {
      v->store(nv, std::memory_order_relaxed);
      changed = true;
    }
  }
  if (active == id && in.released) {
    active = 0;
    in.released = false;
  }
  const float wheel = TakeWheel(r);
  if (wheel != 0) {
    const float t = ParamToNormalized(info, value) + wheel * (in.shift ? 0.005f : 0.025f);
    v->store(ParamFromNormalized(info, t), std::memory_order_relaxed);
    changed = true;
  }
  return changed;
}

bool Ui::Knob(const Rect& r, const ParamInfo& info, std::atomic<float>* v, Color accent, const char* label,
              bool show_value) {
  const WidgetId id = Id(v);
  const bool changed = DragValue(id, r, info, v, 160.0f);
  const float value = v->load(std::memory_order_relaxed);
  const float t = ParamToNormalized(info, value);

  const float label_h = label && !*label ? 0.0f : 13.0f;
  const float value_h = show_value ? 13.0f : 0.0f;
  const float d = std::min(r.w - 6.0f, r.h - label_h - value_h - 2.0f);
  const float cx = r.cx(), cy = r.y + label_h + 1 + d * 0.5f;
  const float rad = d * 0.5f;
  const float kA0 = -2.356f, kA1 = 2.356f;  // +-135 degrees
  const bool engaged = hot == id || active == id;

  if (label_h > 0) g.Text(label ? label : info.name, Rect(r.x - 4, r.y, r.w + 8, 12), theme::kDim, 10.5f, Align::kCenter);
  g.Circle(cx, cy, rad - 1.5f, engaged ? theme::kPanel3 : theme::kPanel2, true);
  g.Arc(cx, cy, rad - 1.5f, kA0, kA1, Mix(theme::kLine, theme::kBg, 0.2f), 3.0f);
  const bool bipolar = info.min < 0 && info.max > 0 && info.unit != ParamUnit::kFader;
  const float a = kA0 + (kA1 - kA0) * t;
  const float origin = bipolar ? kA0 + (kA1 - kA0) * ParamToNormalized(info, 0.0f) : kA0;
  g.Arc(cx, cy, rad - 1.5f, std::min(origin, a), std::max(origin, a), accent, 3.0f);
  const float pr = rad - 5.5f;
  g.Line(cx + sinf(a) * pr * 0.35f, cy - cosf(a) * pr * 0.35f, cx + sinf(a) * pr, cy - cosf(a) * pr,
         engaged ? theme::kText : Mix(theme::kText, theme::kDim, 0.3f), 2.0f);

  char text[32];
  FormatParam(info, value, text, sizeof(text));
  if (show_value) {
    // Narrow knobs (EQ rows) drop the units: "2.5 kHz" -> "2.5k", "+3.0 dB" -> "+3.0", "Q 1.00" -> "1.00".
    char compact[32];
    const char* shown = text;
    if (r.w < 46) {
      std::string s = text;
      auto erase = [&](const char* what, const char* with) {
        size_t at = s.find(what);
        if (at != std::string::npos) s.replace(at, strlen(what), with);
      };
      erase(" kHz", "k");
      erase(" Hz", "");
      erase(" dB", "");
      erase("Q ", "");
      strncpy_s(compact, s.c_str(), _TRUNCATE);
      shown = compact;
    }
    g.Text(shown, Rect(r.x - 6, cy + rad + 1, r.w + 12, 12), engaged ? theme::kText : theme::kDim, 10.5f, Align::kCenter);
  }
  if (engaged && (!show_value || active == id)) {
    char tip[64];
    snprintf(tip, sizeof(tip), "%s  %s", label && *label ? label : info.name, text);
    Tooltip(tip, cx, r.y);
  }
  return changed;
}

bool Ui::Fader(const Rect& r, const ParamInfo& info, std::atomic<float>* v, Color accent) {
  const WidgetId id = Id(v);
  const float track_h = r.h - 16;
  const bool changed = DragValue(id, r, info, v, track_h);
  const float value = v->load(std::memory_order_relaxed);
  const float t = ParamToNormalized(info, value);
  const bool engaged = hot == id || active == id;

  const float cx = r.cx();
  const float y0 = r.y + 8, y1 = r.b() - 8;
  g.FillRound(Rect(cx - 2, y0, 4, y1 - y0), 2, Rgb(0x0b0c10));
  // unity mark
  const float unity_y = y1 - (y1 - y0) * ParamToNormalized(info, 0.0f);
  g.Line(r.x + 1, unity_y, r.r() - 1, unity_y, theme::kFaint, 1.0f);
  const float ty = y1 - (y1 - y0) * t;
  g.FillRound(Rect(cx - 2, ty, 4, y1 - ty), 2, WithAlpha(accent, 0.55f));
  const Rect cap(r.x + 1, ty - 7, r.w - 2, 14);
  g.FillRound(cap, 3, engaged ? Rgb(0xd9dde5) : Rgb(0xb5bbc6));
  g.Line(cap.x + 3, ty, cap.r() - 3, ty, Rgb(0x1a1d24), 1.5f);
  if (engaged) {
    char text[32], tip[64];
    FormatParam(info, value, text, sizeof(text));
    snprintf(tip, sizeof(tip), "%s  %s", info.name, text);
    Tooltip(tip, r.cx(), ty - 8);
  }
  return changed;
}

}
