#pragma once

// Thin Direct2D / DirectWrite wrapper for the studio UI. All coordinates are in DIPs
// (1/96 inch); the render target is created at the window's DPI.

#include <d2d1_1.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <dwrite.h>
#include <map>
#include <string>
#include <unordered_map>

namespace demo {

struct Rect {
  float x = 0, y = 0, w = 0, h = 0;
  Rect() {}
  Rect(float x, float y, float w, float h) : x(x), y(y), w(w), h(h) {}
  float r() const { return x + w; }
  float b() const { return y + h; }
  float cx() const { return x + w * 0.5f; }
  float cy() const { return y + h * 0.5f; }
  bool Contains(float px, float py) const { return px >= x && px < x + w && py >= y && py < y + h; }
  Rect Inset(float d) const { return Rect(x + d, y + d, w - 2 * d, h - 2 * d); }
  Rect Inset(float dx, float dy) const { return Rect(x + dx, y + dy, w - 2 * dx, h - 2 * dy); }
};

struct Color {
  float r, g, b, a;
};
inline Color Rgb(unsigned hex, float a = 1.0f) {
  return Color{((hex >> 16) & 255) / 255.0f, ((hex >> 8) & 255) / 255.0f, (hex & 255) / 255.0f, a};
}
inline Color WithAlpha(Color c, float a) { c.a = a; return c; }
inline Color Mix(Color a, Color b, float t) {
  return Color{a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t, a.b + (b.b - a.b) * t, a.a + (b.a - a.a) * t};
}

enum class Align { kLeft, kCenter, kRight };

class Gfx {
 public:
  Gfx() {}
  ~Gfx() { Destroy(); }
  bool Create(HWND hwnd);
  void Destroy();
  void SetDpi(float dpi);
  float scale() const { return dpi_ / 96.0f; }
  void Resize();  // after WM_SIZE
  bool BeginFrame();
  void EndFrame();
  float width() const { return width_; }
  float height() const { return height_; }

  void Fill(const Rect& r, Color c);
  void FillRound(const Rect& r, float radius, Color c);
  void Stroke(const Rect& r, Color c, float width = 1.0f);
  void StrokeRound(const Rect& r, float radius, Color c, float width = 1.0f);
  void Line(float x0, float y0, float x1, float y1, Color c, float width = 1.0f);
  void Circle(float cx, float cy, float radius, Color c, bool fill, float width = 1.0f);
  // Angles in radians, clockwise from 12 o'clock.
  void Arc(float cx, float cy, float radius, float a0, float a1, Color c, float width);
  void Polyline(const D2D1_POINT_2F* pts, int n, Color c, float width);
  // Polygon from the polyline down to base_y (for spectrum fills).
  void FillUnder(const D2D1_POINT_2F* pts, int n, float base_y, Color c);
  void FillTriangle(float x0, float y0, float x1, float y1, float x2, float y2, Color c);
  void Text(const char* text, const Rect& r, Color c, float size = 12.0f, Align align = Align::kLeft,
            bool bold = false, bool mono = false);
  float TextWidth(const char* text, float size = 12.0f, bool bold = false, bool mono = false);
  void PushClip(const Rect& r);
  void PopClip();

 private:
  IDWriteTextFormat* Format(float size, bool bold, bool mono);
  ID2D1SolidColorBrush* Brush(Color c);
  void DiscardTarget();
  static D2D1_RECT_F ToD2D(const Rect& r) { return D2D1::RectF(r.x, r.y, r.r(), r.b()); }

  HWND hwnd_ = nullptr;
  bool CreateDeviceTarget();     // Direct3D 11 device + flip-model swap chain (preferred)
  bool CreateHwndTarget();       // classic HWND render target (fallback)
  void CreateBackbufferBitmap();
  ID2D1Factory1* factory_ = nullptr;
  ID3D11Device* d3d_ = nullptr;
  ID2D1Device* d2d_device_ = nullptr;
  ID2D1DeviceContext* dc_ = nullptr;
  IDXGISwapChain1* swap_ = nullptr;
  ID2D1Bitmap1* backbuffer_ = nullptr;
  ID2D1HwndRenderTarget* hwnd_target_ = nullptr;
 public:
  bool using_gpu_swapchain() const { return swap_ != nullptr; }
 private:
  IDWriteFactory* dwrite_ = nullptr;
  ID2D1RenderTarget* target_ = nullptr;  // dc_ or hwnd_target_
  ID2D1SolidColorBrush* brush_ = nullptr;
  struct CachedLayout {
    IDWriteTextLayout* layout;
    uint64_t last_used;
  };
  void TrimLayoutCache();
  std::map<int, IDWriteTextFormat*> formats_;
  std::unordered_map<std::string, CachedLayout> layouts_;
  std::string key_;
  uint64_t frame_ = 0;
  float dpi_ = 96.0f;
  float width_ = 0, height_ = 0;
  std::wstring wide_;
};

}
