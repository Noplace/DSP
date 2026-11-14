/*****************************************************************************************************************
* Copyright (c) 2014 Khalid Ali Al-Kooheji                                                                       *
*                                                                                                                *
* Permission is hereby granted, free of charge, to any person obtaining a copy of this software and              *
* associated documentation files (the "Software"), to deal in the Software without restriction, including        *
* without limitation the rights to use, copy, modify, merge, publish, distribute, sublicense, and/or sell        *
* copies of the Software, and to permit persons to whom the Software is furnished to do so, subject to the       *
* following conditions:                                                                                          *
*                                                                                                                *
* The above copyright notice and this permission notice shall be included in all copies or substantial           *
* portions of the Software.                                                                                      *
*                                                                                                                *
* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT          *
* LIMITED TO THE WARRANTIES OF MERCHANTABILITY, * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.          *
* IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, * DAMAGES OR OTHER LIABILITY,      *
* WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE            *
* SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.                                                         *
*****************************************************************************************************************/
#pragma once

#include <atomic>
#include <mmdeviceapi.h>
#include <audioclient.h>

namespace dsp {
namespace audio {
namespace output {

// Low-latency, pull-model audio output using WASAPI shared mode in event-driven mode.
//
// A dedicated thread (registered with MMCSS as "Pro Audio") waits for the audio engine to
// ask for data and calls the render callback with a stereo float buffer to fill. All WASAPI
// objects live on that thread. Uses IAudioClient3's smallest engine period when available
// (typically ~3 ms on Windows 10/11), otherwise the default period (~10 ms).
class WasapiOutput {
 public:
  // Called on the audio thread. Fill frames*2 interleaved stereo floats (L,R,L,R,...).
  // Must not block, allocate or take locks that the UI thread can hold for long.
  typedef void (*RenderCallback)(void* user, float* stereo_out, uint32_t frames);

  WasapiOutput();
  ~WasapiOutput();

  // Opens the default render device and starts streaming. Returns false on failure
  // (see last_error()). The callback starts being called right away.
  bool Start(RenderCallback callback, void* user);
  void Stop();

  bool running() const { return running_; }
  uint32_t sample_rate() const { return sample_rate_; }
  uint32_t channels() const { return channels_; }
  uint32_t period_frames() const { return period_frames_; }
  uint32_t buffer_frames() const { return buffer_frames_; }
  double period_ms() const { return sample_rate_ ? 1000.0 * period_frames_ / sample_rate_ : 0; }
  bool low_latency_mode() const { return low_latency_mode_; }
  // Set if the device went away (unplugged, default device changed...). Playback has stopped.
  bool device_lost() const { return device_lost_; }
  const char* last_error() const { return last_error_; }

 private:
  static DWORD WINAPI ThreadProc(LPVOID param);
  DWORD AudioThread();
  bool OpenDevice();   // audio thread
  void CloseDevice();  // audio thread
  void SetError(const char* what, HRESULT hr);

  RenderCallback callback_;
  void* user_;
  HANDLE thread_;
  HANDLE audio_event_;   // signalled by WASAPI when it wants data
  HANDLE quit_event_;
  HANDLE ready_event_;   // audio thread finished opening the device (successfully or not)

  // WASAPI objects (owned by the audio thread)
  IMMDeviceEnumerator* enumerator_;
  IMMDevice* device_;
  IAudioClient* client_;
  IAudioRenderClient* render_client_;
  WAVEFORMATEX* format_;

  float* scratch_;       // stereo render buffer, buffer_frames_*2
  uint32_t sample_rate_, channels_, period_frames_, buffer_frames_;
  bool low_latency_mode_;
  volatile bool running_;
  std::atomic<bool> device_lost_;
  char last_error_[256];
};

}
}
}
