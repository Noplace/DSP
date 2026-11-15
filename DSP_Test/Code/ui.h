#pragma once

// Small immediate-mode widget layer on top of Gfx: every frame the views call the widget
// functions, which draw and handle the mouse in one go. Widgets bound to mixer parameters
// write straight into the parameter's atomic slot.

#include <atomic>
#include <functional>
#include <string>

namespace demo {

namespace theme {
const Color kBg = Rgb(0x121419);
const Color kPanel = Rgb(0x1a1d24);
const Color kPanel2 = Rgb(0x22262f);
const Color kPanel3 = Rgb(0x2b303b);
const Color kLine = Rgb(0x313744);
const Color kText = Rgb(0xe7e9ee);
const Color kDim = Rgb(0x8d95a4);
const Color kFaint = Rgb(0x5c6473);
const Color kAccent = Rgb(0x4ea1ff);
const Color kGreen = Rgb(0x3ddc84);
const Color kYellow = Rgb(0xffd24a);
const Color kOrange = Rgb(0xff9f43);
const Color kRed = Rgb(0xff5a5a);
Color Channel(int index);  // per-channel colour (index 16 = master)
}

struct Input {
  float mx = -1000, my = -1000;
  bool down = false;
  bool pressed = false, released = false, dbl = false;  // since the last frame
  bool rpressed = false;
  float wheel = 0;  // notches since the last frame (+ = away from the user)
  bool shift = false, ctrl = false;
};

using WidgetId = uintptr_t;
inline WidgetId Id(const void* p, int salt = 0) { return reinterpret_cast<uintptr_t>(p) * 31 + uintptr_t(salt); }

class Ui {
 public:
  explicit Ui(Gfx& g) : g(g) {}
  Gfx& g;
  Input in;
  WidgetId active = 0;  // widget being dragged / pressed
  WidgetId hot = 0;     // widget under the mouse this frame
  bool knob_wheel = true;  // knobs/faders react to the wheel (off while a panel is being scrolled)
  // Parameter widgets: the one being dragged, the one right-clicked this frame (for a context
  // menu), and per-parameter badges (kFlag* bits) drawn on knobs and faders.
  std::atomic<float>* active_slot = nullptr;
  std::atomic<float>* context_slot = nullptr;
  float context_x = 0, context_y = 0;
  enum { kFlagAutomated = 1, kFlagMapped = 2, kFlagLearning = 4 };
  std::function<uint32_t(const std::atomic<float>*)> slot_flags;

  void BeginFrame();
  void EndFrame();  // draws the tooltip, clears per-frame input
  bool Hover(const Rect& r) const {
    return r.Contains(in.mx, in.my) && (!clipped_ || clip_.Contains(in.mx, in.my)) && (!modal_ || in_modal_);
  }
  // While a popup is open, only widgets drawn between BeginModal/EndModal take input.
  void SetModal(bool on) { modal_ = on; }
  void BeginModal() { in_modal_ = true; }
  void EndModal() { in_modal_ = false; }
  // Clip drawing and hit-testing (for scrolled panels). Not nestable.
  void Clip(const Rect& r) { clip_ = r; clipped_ = true; g.PushClip(r); }
  void Unclip() { if (clipped_) { clipped_ = false; g.PopClip(); } }
  // Mouse over r and not captured by another widget.
  bool Over(WidgetId id, const Rect& r) const { return Hover(r) && (active == 0 || active == id); }
  // Press/drag/release on an area. Returns true on the frame the press starts.
  bool Press(WidgetId id, const Rect& r);
  bool Dragging(WidgetId id) const { return active == id && in.down; }
  bool Clicked(WidgetId id, const Rect& r);  // released over the area after pressing it
  float TakeWheel(const Rect& r);            // wheel notches if over r (consumes them)

  bool Button(WidgetId id, const Rect& r, const char* label, bool on = false, Color on_color = theme::kAccent,
              float text_size = 12.0f);
  // Toggle bound to a switch parameter. Returns true when it changed.
  bool Switch(WidgetId id, const Rect& r, const char* label, std::atomic<float>* v, Color on_color = theme::kAccent);
  // Small round power button for a section's on/off parameter.
  bool Power(WidgetId id, float cx, float cy, std::atomic<float>* v, Color on_color);
  // Rotary knob: drag up/down (Shift = fine), wheel, double-click = default.
  bool Knob(const Rect& r, const dsp::audio::synth::effects::ParamInfo& info, std::atomic<float>* v, Color accent,
            const char* label = nullptr, bool show_value = true);
  // Compact "label value" box: drag up/down, wheel, double-click = default.
  bool ValueBox(const Rect& r, const dsp::audio::synth::effects::ParamInfo& info, std::atomic<float>* v, Color accent,
                const char* label);
  // Vertical fader for a level parameter.
  bool Fader(const Rect& r, const dsp::audio::synth::effects::ParamInfo& info, std::atomic<float>* v, Color accent);

  void Tooltip(const char* text, float x, float y);

 private:
  bool DragValue(WidgetId id, const Rect& r, const dsp::audio::synth::effects::ParamInfo& info,
                 std::atomic<float>* v, float pixels_per_range);
  void DrawBadges(const Rect& r, const std::atomic<float>* v);
  float drag_y0_ = 0, drag_t0_ = 0;
  Rect clip_;
  bool clipped_ = false;
  bool modal_ = false, in_modal_ = false;
  std::string tip_;
  float tip_x_ = 0, tip_y_ = 0;
};

}
