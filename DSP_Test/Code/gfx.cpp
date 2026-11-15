#include "global.h"

#pragma comment(lib, "d2d1.lib")
#pragma comment(lib, "dwrite.lib")
#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")

namespace demo {

template <typename T>
static void SafeRelease(T** p) {
  if (*p) {
    (*p)->Release();
    *p = nullptr;
  }
}

bool Gfx::Create(HWND hwnd) {
  hwnd_ = hwnd;
  if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, __uuidof(ID2D1Factory1), nullptr,
                               reinterpret_cast<void**>(&factory_))))
    return false;
  if (FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                                 reinterpret_cast<IUnknown**>(&dwrite_))))
    return false;
  return true;
}

void Gfx::Destroy() {
  DiscardTarget();
  for (auto& l : layouts_) l.second.layout->Release();
  layouts_.clear();
  for (auto& f : formats_) if (f.second) f.second->Release();
  formats_.clear();
  SafeRelease(&dwrite_);
  SafeRelease(&factory_);
}

void Gfx::DiscardTarget() {
  SafeRelease(&brush_);
  if (dc_) dc_->SetTarget(nullptr);
  SafeRelease(&backbuffer_);
  SafeRelease(&swap_);
  SafeRelease(&dc_);
  SafeRelease(&d2d_device_);
  SafeRelease(&d3d_);
  SafeRelease(&hwnd_target_);
  target_ = nullptr;  // not owned: it is dc_ or hwnd_target_
}

void Gfx::SetDpi(float dpi) {
  dpi_ = dpi;
  if (target_) target_->SetDpi(dpi_, dpi_);
  Resize();
}

void Gfx::CreateBackbufferBitmap() {
  IDXGISurface* surface = nullptr;
  if (FAILED(swap_->GetBuffer(0, IID_PPV_ARGS(&surface)))) return;
  const auto props = D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
                                             D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE),
                                             dpi_, dpi_);
  dc_->CreateBitmapFromDxgiSurface(surface, &props, &backbuffer_);
  surface->Release();
  dc_->SetTarget(backbuffer_);
}

void Gfx::Resize() {
  RECT rc;
  GetClientRect(hwnd_, &rc);
  const UINT w = std::max<LONG>(1, rc.right - rc.left), h = std::max<LONG>(1, rc.bottom - rc.top);
  if (swap_) {
    dc_->SetTarget(nullptr);
    SafeRelease(&backbuffer_);
    if (FAILED(swap_->ResizeBuffers(0, w, h, DXGI_FORMAT_UNKNOWN, 0))) {
      DiscardTarget();  // rebuilt on the next frame
    } else {
      CreateBackbufferBitmap();
    }
  } else if (hwnd_target_) {
    hwnd_target_->Resize(D2D1::SizeU(w, h));
  }
  width_ = (rc.right - rc.left) / scale();
  height_ = (rc.bottom - rc.top) / scale();
}

// Direct2D on a Direct3D 11 device presenting through a flip-model swap chain: the GPU does the
// rasterising and composition, which takes a good part of the per-frame CPU cost off the UI
// thread compared with the HWND render target.
bool Gfx::CreateDeviceTarget() {
  const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1,
                                      D3D_FEATURE_LEVEL_10_0};
  if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, levels,
                               UINT(std::size(levels)), D3D11_SDK_VERSION, &d3d_, nullptr, nullptr)))
    return false;
  IDXGIDevice* dxgi = nullptr;
  IDXGIAdapter* adapter = nullptr;
  IDXGIFactory2* dxgi_factory = nullptr;
  bool ok = SUCCEEDED(d3d_->QueryInterface(IID_PPV_ARGS(&dxgi))) &&
            SUCCEEDED(factory_->CreateDevice(dxgi, &d2d_device_)) &&
            SUCCEEDED(d2d_device_->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &dc_)) &&
            SUCCEEDED(dxgi->GetAdapter(&adapter)) && SUCCEEDED(adapter->GetParent(IID_PPV_ARGS(&dxgi_factory)));
  if (ok) {
    RECT rc;
    GetClientRect(hwnd_, &rc);
    DXGI_SWAP_CHAIN_DESC1 desc = {};
    desc.Width = std::max<LONG>(1, rc.right - rc.left);
    desc.Height = std::max<LONG>(1, rc.bottom - rc.top);
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.BufferCount = 2;
    desc.Scaling = DXGI_SCALING_NONE;
    desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
    ok = SUCCEEDED(dxgi_factory->CreateSwapChainForHwnd(d3d_, hwnd_, &desc, nullptr, nullptr, &swap_));
    if (ok) {
      dxgi_factory->MakeWindowAssociation(hwnd_, DXGI_MWA_NO_ALT_ENTER);
      dc_->SetDpi(dpi_, dpi_);
      CreateBackbufferBitmap();
      ok = backbuffer_ != nullptr;
    }
  }
  if (dxgi_factory) dxgi_factory->Release();
  if (adapter) adapter->Release();
  if (dxgi) dxgi->Release();
  if (!ok) {
    DiscardTarget();
    return false;
  }
  target_ = dc_;
  return true;
}

bool Gfx::CreateHwndTarget() {
  RECT rc;
  GetClientRect(hwnd_, &rc);
  auto props = D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_DEFAULT,
                                            D2D1::PixelFormat(DXGI_FORMAT_UNKNOWN, D2D1_ALPHA_MODE_UNKNOWN), dpi_, dpi_);
  auto hwnd_props = D2D1::HwndRenderTargetProperties(hwnd_, D2D1::SizeU(rc.right - rc.left, rc.bottom - rc.top));
  if (FAILED(factory_->CreateHwndRenderTarget(props, hwnd_props, &hwnd_target_)))
    return false;
  target_ = hwnd_target_;
  return true;
}

bool Gfx::BeginFrame() {
  if (!target_) {
    // DSP_GFX=hwnd forces the old render target (for comparisons).
    const char* mode = getenv("DSP_GFX");
    const bool force_hwnd = mode && _stricmp(mode, "hwnd") == 0;
    if ((force_hwnd || !CreateDeviceTarget()) && !CreateHwndTarget())
      return false;
    target_->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
    if (FAILED(target_->CreateSolidColorBrush(D2D1::ColorF(1, 1, 1, 1), &brush_))) {
      DiscardTarget();
      return false;
    }
    Resize();
    if (!target_) return false;
  }
  target_->BeginDraw();
  target_->SetTransform(D2D1::Matrix3x2F::Identity());
  return true;
}

void Gfx::EndFrame() {
  TrimLayoutCache();
  if (!target_) return;
  HRESULT hr = target_->EndDraw();
  if (SUCCEEDED(hr) && swap_) hr = swap_->Present(1, 0);  // waits for vblank, like the HWND target
  if (hr == D2DERR_RECREATE_TARGET || hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET)
    DiscardTarget();  // device lost: rebuilt on the next frame
}

ID2D1SolidColorBrush* Gfx::Brush(Color c) {
  brush_->SetColor(D2D1::ColorF(c.r, c.g, c.b, c.a));
  return brush_;
}

void Gfx::Fill(const Rect& r, Color c) { target_->FillRectangle(ToD2D(r), Brush(c)); }

void Gfx::FillRound(const Rect& r, float radius, Color c) {
  target_->FillRoundedRectangle(D2D1::RoundedRect(ToD2D(r), radius, radius), Brush(c));
}

void Gfx::Stroke(const Rect& r, Color c, float width) {
  target_->DrawRectangle(ToD2D(r.Inset(width * 0.5f)), Brush(c), width);
}

void Gfx::StrokeRound(const Rect& r, float radius, Color c, float width) {
  target_->DrawRoundedRectangle(D2D1::RoundedRect(ToD2D(r.Inset(width * 0.5f)), radius, radius), Brush(c), width);
}

void Gfx::Line(float x0, float y0, float x1, float y1, Color c, float width) {
  target_->DrawLine(D2D1::Point2F(x0, y0), D2D1::Point2F(x1, y1), Brush(c), width);
}

void Gfx::Circle(float cx, float cy, float radius, Color c, bool fill, float width) {
  auto e = D2D1::Ellipse(D2D1::Point2F(cx, cy), radius, radius);
  if (fill) target_->FillEllipse(e, Brush(c));
  else target_->DrawEllipse(e, Brush(c), width);
}

void Gfx::Arc(float cx, float cy, float radius, float a0, float a1, Color c, float width) {
  if (fabsf(a1 - a0) < 1e-3f) return;
  if (a1 < a0) std::swap(a0, a1);
  ID2D1PathGeometry* path = nullptr;
  if (FAILED(factory_->CreatePathGeometry(&path))) return;
  ID2D1GeometrySink* sink = nullptr;
  if (SUCCEEDED(path->Open(&sink))) {
    auto pt = [&](float a) { return D2D1::Point2F(cx + sinf(a) * radius, cy - cosf(a) * radius); };
    sink->BeginFigure(pt(a0), D2D1_FIGURE_BEGIN_HOLLOW);
    sink->AddArc(D2D1::ArcSegment(pt(a1), D2D1::SizeF(radius, radius), 0, D2D1_SWEEP_DIRECTION_CLOCKWISE,
                                  a1 - a0 > 3.14159265f ? D2D1_ARC_SIZE_LARGE : D2D1_ARC_SIZE_SMALL));
    sink->EndFigure(D2D1_FIGURE_END_OPEN);
    sink->Close();
    sink->Release();
    target_->DrawGeometry(path, Brush(c), width);
  }
  path->Release();
}

void Gfx::Polyline(const D2D1_POINT_2F* pts, int n, Color c, float width) {
  if (n < 2) return;
  ID2D1PathGeometry* path = nullptr;
  if (FAILED(factory_->CreatePathGeometry(&path))) return;
  ID2D1GeometrySink* sink = nullptr;
  if (SUCCEEDED(path->Open(&sink))) {
    sink->BeginFigure(pts[0], D2D1_FIGURE_BEGIN_HOLLOW);
    sink->AddLines(pts + 1, n - 1);
    sink->EndFigure(D2D1_FIGURE_END_OPEN);
    sink->Close();
    sink->Release();
    target_->DrawGeometry(path, Brush(c), width);
  }
  path->Release();
}

void Gfx::FillUnder(const D2D1_POINT_2F* pts, int n, float base_y, Color c) {
  if (n < 2) return;
  ID2D1PathGeometry* path = nullptr;
  if (FAILED(factory_->CreatePathGeometry(&path))) return;
  ID2D1GeometrySink* sink = nullptr;
  if (SUCCEEDED(path->Open(&sink))) {
    sink->BeginFigure(D2D1::Point2F(pts[0].x, base_y), D2D1_FIGURE_BEGIN_FILLED);
    sink->AddLines(pts, n);
    sink->AddLine(D2D1::Point2F(pts[n - 1].x, base_y));
    sink->EndFigure(D2D1_FIGURE_END_CLOSED);
    sink->Close();
    sink->Release();
    target_->FillGeometry(path, Brush(c));
  }
  path->Release();
}

void Gfx::FillTriangle(float x0, float y0, float x1, float y1, float x2, float y2, Color c) {
  D2D1_POINT_2F p[3] = {{x0, y0}, {x1, y1}, {x2, y2}};
  ID2D1PathGeometry* path = nullptr;
  if (FAILED(factory_->CreatePathGeometry(&path))) return;
  ID2D1GeometrySink* sink = nullptr;
  if (SUCCEEDED(path->Open(&sink))) {
    sink->BeginFigure(p[0], D2D1_FIGURE_BEGIN_FILLED);
    sink->AddLines(p + 1, 2);
    sink->EndFigure(D2D1_FIGURE_END_CLOSED);
    sink->Close();
    sink->Release();
    target_->FillGeometry(path, Brush(c));
  }
  path->Release();
}

IDWriteTextFormat* Gfx::Format(float size, bool bold, bool mono) {
  const int key = int(size * 4) * 4 + (bold ? 1 : 0) + (mono ? 2 : 0);
  auto it = formats_.find(key);
  if (it != formats_.end()) return it->second;
  IDWriteTextFormat* format = nullptr;
  dwrite_->CreateTextFormat(mono ? L"Consolas" : L"Segoe UI", nullptr,
                            bold ? DWRITE_FONT_WEIGHT_SEMI_BOLD : DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL,
                            DWRITE_FONT_STRETCH_NORMAL, size, L"en-us", &format);
  if (format) {
    format->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    format->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    IDWriteInlineObject* ellipsis = nullptr;
    dwrite_->CreateEllipsisTrimmingSign(format, &ellipsis);
    DWRITE_TRIMMING trimming = {DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0};
    format->SetTrimming(&trimming, ellipsis);
    if (ellipsis) ellipsis->Release();  // the format keeps its own reference
  }
  formats_[key] = format;
  return format;
}

static void Widen(const char* text, std::wstring& out) {
  const int n = MultiByteToWideChar(CP_UTF8, 0, text, -1, nullptr, 0);
  out.resize(n > 0 ? n - 1 : 0);
  if (n > 1) MultiByteToWideChar(CP_UTF8, 0, text, -1, &out[0], n);
}

// Laying text out is by far the most expensive part of a frame, and almost every string is the
// same as last frame, so layouts are cached by (text, font, alignment, box size) and dropped once
// they have not been used for a couple of seconds.
void Gfx::Text(const char* text, const Rect& r, Color c, float size, Align align, bool bold, bool mono) {
  if (!text || !*text || r.w <= 0 || r.h <= 0) return;
  key_.assign(text);
  char tail[48];
  snprintf(tail, sizeof(tail), "\x01%d|%d|%d|%d|%d", int(size * 4), (bold ? 1 : 0) | (mono ? 2 : 0) | (int(align) << 2),
           int(r.w * 4), int(r.h * 4), 0);
  key_ += tail;
  auto it = layouts_.find(key_);
  if (it == layouts_.end()) {
    auto format = Format(size, bold, mono);
    if (!format) return;
    Widen(text, wide_);
    format->SetTextAlignment(align == Align::kLeft     ? DWRITE_TEXT_ALIGNMENT_LEADING
                             : align == Align::kCenter ? DWRITE_TEXT_ALIGNMENT_CENTER
                                                       : DWRITE_TEXT_ALIGNMENT_TRAILING);
    IDWriteTextLayout* layout = nullptr;
    if (FAILED(dwrite_->CreateTextLayout(wide_.c_str(), UINT32(wide_.size()), format, r.w, r.h, &layout)))
      return;
    it = layouts_.emplace(key_, CachedLayout{layout, frame_}).first;
  }
  it->second.last_used = frame_;
  target_->DrawTextLayout(D2D1::Point2F(r.x, r.y), it->second.layout, Brush(c), D2D1_DRAW_TEXT_OPTIONS_NONE);
}

void Gfx::TrimLayoutCache() {
  ++frame_;
  if (frame_ % 60 != 0) return;
  for (auto it = layouts_.begin(); it != layouts_.end();) {
    if (frame_ - it->second.last_used > 120) {
      it->second.layout->Release();
      it = layouts_.erase(it);
    } else {
      ++it;
    }
  }
}

float Gfx::TextWidth(const char* text, float size, bool bold, bool mono) {
  auto format = Format(size, bold, mono);
  if (!format || !text || !*text) return 0;
  Widen(text, wide_);
  IDWriteTextLayout* layout = nullptr;
  if (FAILED(dwrite_->CreateTextLayout(wide_.c_str(), UINT32(wide_.size()), format, 10000.0f, 100.0f, &layout)))
    return 0;
  DWRITE_TEXT_METRICS m = {};
  layout->GetMetrics(&m);
  layout->Release();
  return m.widthIncludingTrailingWhitespace;
}

void Gfx::PushClip(const Rect& r) { target_->PushAxisAlignedClip(ToD2D(r), D2D1_ANTIALIAS_MODE_ALIASED); }
void Gfx::PopClip() { target_->PopAxisAlignedClip(); }

}
