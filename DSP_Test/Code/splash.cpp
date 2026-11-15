#include "global.h"

namespace demo {

static const char kSplashClass[] = "DSP_Studio_Splash";

void Splash::Show(const char* status) {
  status_ = status;
  WNDCLASSEXA wc = {sizeof(wc)};
  wc.lpfnWndProc = Proc;
  wc.hInstance = GetModuleHandle(nullptr);
  wc.lpszClassName = kSplashClass;
  wc.hCursor = LoadCursor(nullptr, IDC_APPSTARTING);
  RegisterClassExA(&wc);
  scale_ = GetDpiForSystem() / 96.0f;
  const int w = int(440 * scale_), h = int(200 * scale_);
  RECT work;
  SystemParametersInfo(SPI_GETWORKAREA, 0, &work, 0);
  hwnd_ = CreateWindowExA(WS_EX_TOOLWINDOW | WS_EX_TOPMOST, kSplashClass, "DSP Studio", WS_POPUP,
                          work.left + (work.right - work.left - w) / 2, work.top + (work.bottom - work.top - h) / 2, w,
                          h, nullptr, nullptr, wc.hInstance, this);
  if (!hwnd_) return;
  SetWindowLongPtr(hwnd_, GWLP_USERDATA, (LONG_PTR)this);
  ShowWindow(hwnd_, SW_SHOWNORMAL);
  UpdateWindow(hwnd_);
}

void Splash::Close() {
  if (hwnd_) {
    DestroyWindow(hwnd_);
    hwnd_ = nullptr;
    UnregisterClassA(kSplashClass, GetModuleHandle(nullptr));
  }
}

LRESULT CALLBACK Splash::Proc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
  auto self = reinterpret_cast<Splash*>(GetWindowLongPtr(hwnd, GWLP_USERDATA));
  switch (msg) {
    case WM_PAINT:
      if (self) self->Paint();
      else ValidateRect(hwnd, nullptr);
      return 0;
    case WM_ERASEBKGND:
      return 1;
  }
  return DefWindowProc(hwnd, msg, wparam, lparam);
}

// Double-buffered GDI: title, status line and an indeterminate progress bar.
void Splash::Paint() {
  PAINTSTRUCT ps;
  HDC dc = BeginPaint(hwnd_, &ps);
  RECT rc;
  GetClientRect(hwnd_, &rc);
  const int w = rc.right, h = rc.bottom;
  HDC mem = CreateCompatibleDC(dc);
  HBITMAP bmp = CreateCompatibleBitmap(dc, w, h);
  HGDIOBJ old_bmp = SelectObject(mem, bmp);
  auto px = [&](float v) { return int(v * scale_ + 0.5f); };
  auto fill = [&](int l, int t, int r, int b, COLORREF c) {
    RECT box = {l, t, r, b};
    HBRUSH brush = CreateSolidBrush(c);
    FillRect(mem, &box, brush);
    DeleteObject(brush);
  };
  fill(0, 0, w, h, RGB(0x1a, 0x1d, 0x24));
  fill(0, 0, w, px(3), RGB(0x4e, 0xa1, 0xff));  // accent line along the top
  // border
  HBRUSH frame = CreateSolidBrush(RGB(0x31, 0x37, 0x44));
  FrameRect(mem, &rc, frame);
  DeleteObject(frame);

  SetBkMode(mem, TRANSPARENT);
  auto font = [&](int size, int weight) {
    return CreateFontA(-px(float(size)), 0, 0, 0, weight, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                       CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, "Segoe UI");
  };
  HFONT title = font(30, FW_SEMIBOLD), sub = font(13, FW_NORMAL), status_font = font(12, FW_NORMAL);
  HGDIOBJ old_font = SelectObject(mem, title);
  SetTextColor(mem, RGB(0x4e, 0xa1, 0xff));
  TextOutA(mem, px(32), px(36), "DSP", 3);
  SIZE dsp_size;
  GetTextExtentPoint32A(mem, "DSP ", 4, &dsp_size);
  SetTextColor(mem, RGB(0xe7, 0xe9, 0xee));
  TextOutA(mem, px(32) + dsp_size.cx, px(36), "Studio", 6);
  SelectObject(mem, sub);
  SetTextColor(mem, RGB(0x8d, 0x95, 0xa4));
  const char* tagline = "MIDI synth, mixer and effects";
  TextOutA(mem, px(34), px(84), tagline, int(strlen(tagline)));
  SelectObject(mem, status_font);
  SetTextColor(mem, RGB(0xc8, 0xcc, 0xd4));
  TextOutA(mem, px(34), px(134), status_.c_str(), int(status_.size()));

  // indeterminate bar: a block sliding along the track
  const int bx = px(34), by = px(160), bw = w - 2 * px(34), bh = px(4);
  fill(bx, by, bx + bw, by + bh, RGB(0x0b, 0x0c, 0x10));
  const int block = bw / 4;
  const int travel = bw + block;
  const int pos = (phase_ * px(6)) % travel - block;
  fill(bx + std::max(0, pos), by, bx + std::min(bw, pos + block), by + bh, RGB(0x4e, 0xa1, 0xff));

  BitBlt(dc, 0, 0, w, h, mem, 0, 0, SRCCOPY);
  SelectObject(mem, old_font);
  DeleteObject(title);
  DeleteObject(sub);
  DeleteObject(status_font);
  SelectObject(mem, old_bmp);
  DeleteObject(bmp);
  DeleteDC(mem);
  EndPaint(hwnd_, &ps);
}

}
