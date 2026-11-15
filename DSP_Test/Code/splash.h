#pragma once

// Small startup window shown while the audio device opens and the instruments (including the
// calibrated piano and harpsichord) load, which takes a few seconds.

#include <atomic>
#include <string>
#include <thread>

namespace demo {

class Splash {
 public:
  ~Splash() { Close(); }
  void Show(const char* status);
  void Close();
  // Runs work() on a worker thread and keeps the splash painted and animated until it returns.
  template <typename F>
  void RunWhile(F&& work) {
    std::atomic<bool> done{false};
    std::thread worker([&] {
      work();
      done = true;
      if (hwnd_) PostMessage(hwnd_, WM_NULL, 0, 0);  // wake the loop below
    });
    while (!done) {
      MsgWaitForMultipleObjects(0, nullptr, FALSE, 33, QS_ALLINPUT);
      MSG msg;
      while (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
      }
      ++phase_;
      if (hwnd_) InvalidateRect(hwnd_, nullptr, FALSE);
    }
    worker.join();
  }

 private:
  static LRESULT CALLBACK Proc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam);
  void Paint();
  HWND hwnd_ = nullptr;
  int phase_ = 0;
  float scale_ = 1.0f;
  std::string status_;
};

}
