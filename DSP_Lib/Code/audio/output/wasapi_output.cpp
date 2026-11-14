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
#include <stdio.h>
#include "../../dsp.h"
#include <avrt.h>

#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "avrt.lib")

namespace dsp {
namespace audio {
namespace output {

namespace {

// KSDATAFORMAT_SUBTYPE_IEEE_FLOAT, spelled out to avoid pulling in ksmedia.h GUID plumbing.
const GUID kSubtypeIeeeFloat = {0x00000003, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}};

bool IsFloat32(const WAVEFORMATEX* wf) {
  if (wf->wBitsPerSample != 32)
    return false;
  if (wf->wFormatTag == WAVE_FORMAT_IEEE_FLOAT)
    return true;
  if (wf->wFormatTag == WAVE_FORMAT_EXTENSIBLE && wf->cbSize >= 22) {
    auto wfx = reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(wf);
    return IsEqualGUID(wfx->SubFormat, kSubtypeIeeeFloat) != 0;
  }
  return false;
}

inline float ClampSample(float v) {
  return v > 1.0f ? 1.0f : (v < -1.0f ? -1.0f : v);
}

}  // namespace

WasapiOutput::WasapiOutput()
    : callback_(nullptr), user_(nullptr), thread_(nullptr), audio_event_(nullptr), quit_event_(nullptr),
      ready_event_(nullptr), enumerator_(nullptr), device_(nullptr), client_(nullptr), render_client_(nullptr),
      format_(nullptr), scratch_(nullptr), sample_rate_(0), channels_(0), period_frames_(0), buffer_frames_(0),
      low_latency_mode_(false), running_(false), device_lost_(false) {
  last_error_[0] = 0;
}

WasapiOutput::~WasapiOutput() {
  Stop();
}

bool WasapiOutput::Start(RenderCallback callback, void* user) {
  if (running_)
    return true;
  callback_ = callback;
  user_ = user;
  device_lost_ = false;
  last_error_[0] = 0;

  audio_event_ = CreateEvent(nullptr, FALSE, FALSE, nullptr);
  quit_event_ = CreateEvent(nullptr, TRUE, FALSE, nullptr);
  ready_event_ = CreateEvent(nullptr, TRUE, FALSE, nullptr);
  thread_ = CreateThread(nullptr, 0, ThreadProc, this, 0, nullptr);
  if (thread_ == nullptr) {
    SetError("CreateThread", HRESULT_FROM_WIN32(GetLastError()));
    Stop();
    return false;
  }

  // Wait until the audio thread has opened the device (or failed to).
  HANDLE handles[2] = {ready_event_, thread_};
  WaitForMultipleObjects(2, handles, FALSE, INFINITE);
  if (!running_) {
    Stop();
    return false;
  }
  return true;
}

void WasapiOutput::Stop() {
  if (thread_ != nullptr) {
    SetEvent(quit_event_);
    WaitForSingleObject(thread_, INFINITE);
    CloseHandle(thread_);
    thread_ = nullptr;
  }
  if (audio_event_) { CloseHandle(audio_event_); audio_event_ = nullptr; }
  if (quit_event_) { CloseHandle(quit_event_); quit_event_ = nullptr; }
  if (ready_event_) { CloseHandle(ready_event_); ready_event_ = nullptr; }
  running_ = false;
}

void WasapiOutput::SetError(const char* what, HRESULT hr) {
  sprintf_s(last_error_, "WASAPI: %s failed (0x%08X)", what, (unsigned)hr);
  OutputDebugStringA(last_error_);
  OutputDebugStringA("\n");
}

DWORD WINAPI WasapiOutput::ThreadProc(LPVOID param) {
  return static_cast<WasapiOutput*>(param)->AudioThread();
}

bool WasapiOutput::OpenDevice() {
  HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&enumerator_));
  if (FAILED(hr)) { SetError("CoCreateInstance(MMDeviceEnumerator)", hr); return false; }

  hr = enumerator_->GetDefaultAudioEndpoint(eRender, eConsole, &device_);
  if (FAILED(hr)) { SetError("GetDefaultAudioEndpoint", hr); return false; }

  // Prefer IAudioClient3 (Windows 10+) for small engine periods.
  IAudioClient3* client3 = nullptr;
  hr = device_->Activate(__uuidof(IAudioClient3), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(&client3));
  if (SUCCEEDED(hr)) {
    client_ = client3;
  } else {
    client3 = nullptr;
    hr = device_->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(&client_));
    if (FAILED(hr)) { SetError("IMMDevice::Activate", hr); return false; }
  }

  hr = client_->GetMixFormat(&format_);
  if (FAILED(hr)) { SetError("GetMixFormat", hr); return false; }

  bool initialized = false;
  if (IsFloat32(format_)) {
    // Shared-mode mix format is float32 on any modern Windows; use it as-is (no conversion).
    if (client3 != nullptr) {
      UINT32 default_period = 0, fundamental = 0, min_period = 0, max_period = 0;
      hr = client3->GetSharedModeEnginePeriod(format_, &default_period, &fundamental, &min_period, &max_period);
      if (SUCCEEDED(hr)) {
        hr = client3->InitializeSharedAudioStream(AUDCLNT_STREAMFLAGS_EVENTCALLBACK, min_period, format_, nullptr);
        if (SUCCEEDED(hr)) {
          initialized = true;
          low_latency_mode_ = min_period < default_period;
        }
      }
    }
    if (!initialized) {
      REFERENCE_TIME default_period = 0, min_period = 0;
      client_->GetDevicePeriod(&default_period, &min_period);
      hr = client_->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_EVENTCALLBACK, default_period, 0, format_, nullptr);
      if (FAILED(hr)) { SetError("IAudioClient::Initialize", hr); return false; }
      initialized = true;
    }
  } else {
    // Unusual mix format: ask the engine to convert from stereo float for us.
    WAVEFORMATEXTENSIBLE wfx = {};
    wfx.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
    wfx.Format.nChannels = 2;
    wfx.Format.nSamplesPerSec = format_->nSamplesPerSec;
    wfx.Format.wBitsPerSample = 32;
    wfx.Format.nBlockAlign = 2 * 4;
    wfx.Format.nAvgBytesPerSec = wfx.Format.nSamplesPerSec * wfx.Format.nBlockAlign;
    wfx.Format.cbSize = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
    wfx.Samples.wValidBitsPerSample = 32;
    wfx.dwChannelMask = SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT;
    wfx.SubFormat = kSubtypeIeeeFloat;

    REFERENCE_TIME default_period = 0, min_period = 0;
    client_->GetDevicePeriod(&default_period, &min_period);
    hr = client_->Initialize(AUDCLNT_SHAREMODE_SHARED,
                             AUDCLNT_STREAMFLAGS_EVENTCALLBACK | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY,
                             default_period, 0, &wfx.Format, nullptr);
    if (FAILED(hr)) { SetError("IAudioClient::Initialize (autoconvert)", hr); return false; }
    CoTaskMemFree(format_);
    format_ = static_cast<WAVEFORMATEX*>(CoTaskMemAlloc(sizeof(wfx)));
    memcpy(format_, &wfx, sizeof(wfx));
  }

  sample_rate_ = format_->nSamplesPerSec;
  channels_ = format_->nChannels;

  hr = client_->SetEventHandle(audio_event_);
  if (FAILED(hr)) { SetError("SetEventHandle", hr); return false; }

  UINT32 buffer_frames = 0;
  hr = client_->GetBufferSize(&buffer_frames);
  if (FAILED(hr)) { SetError("GetBufferSize", hr); return false; }
  buffer_frames_ = buffer_frames;

  if (client3 != nullptr) {
    UINT32 current_period = 0;
    WAVEFORMATEX* current_format = nullptr;
    if (SUCCEEDED(client3->GetCurrentSharedModeEnginePeriod(&current_format, &current_period))) {
      period_frames_ = current_period;
      CoTaskMemFree(current_format);
    }
  }
  if (period_frames_ == 0) {
    REFERENCE_TIME default_period = 0, min_period = 0;
    client_->GetDevicePeriod(&default_period, &min_period);
    period_frames_ = uint32_t((default_period * (int64_t)sample_rate_ + 5000000) / 10000000);
  }

  hr = client_->GetService(IID_PPV_ARGS(&render_client_));
  if (FAILED(hr)) { SetError("GetService(IAudioRenderClient)", hr); return false; }

  scratch_ = new float[buffer_frames_ * 2];

  // Start with a buffer of silence so the first period doesn't glitch.
  BYTE* data = nullptr;
  if (SUCCEEDED(render_client_->GetBuffer(buffer_frames_, &data)))
    render_client_->ReleaseBuffer(buffer_frames_, AUDCLNT_BUFFERFLAGS_SILENT);

  hr = client_->Start();
  if (FAILED(hr)) { SetError("IAudioClient::Start", hr); return false; }

  char info[200];
  sprintf_s(info, "WASAPI: %u Hz, %u ch, period %u frames (%.2f ms), buffer %u frames, %s\n", sample_rate_, channels_,
            period_frames_, period_ms(), buffer_frames_, low_latency_mode_ ? "IAudioClient3 low-latency" : "default period");
  OutputDebugStringA(info);
  return true;
}

void WasapiOutput::CloseDevice() {
  if (client_) client_->Stop();
  SafeRelease(&render_client_);
  SafeRelease(&client_);
  SafeRelease(&device_);
  SafeRelease(&enumerator_);
  if (format_) { CoTaskMemFree(format_); format_ = nullptr; }
  SafeDeleteArray(&scratch_);
}

DWORD WasapiOutput::AudioThread() {
  HRESULT com_hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);

  DWORD task_index = 0;
  HANDLE mmcss = AvSetMmThreadCharacteristicsW(L"Pro Audio", &task_index);
  if (mmcss == nullptr)
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);

  running_ = OpenDevice();
  SetEvent(ready_event_);

  if (running_) {
    HANDLE handles[2] = {quit_event_, audio_event_};
    for (;;) {
      DWORD wait = WaitForMultipleObjects(2, handles, FALSE, 2000);
      if (wait == WAIT_OBJECT_0)
        break;  // quit
      if (wait != WAIT_OBJECT_0 + 1)
        continue;  // timeout: engine idle, keep waiting

      UINT32 padding = 0;
      HRESULT hr = client_->GetCurrentPadding(&padding);
      if (hr == AUDCLNT_E_DEVICE_INVALIDATED) { device_lost_ = true; SetError("device lost", hr); break; }
      if (FAILED(hr)) continue;

      uint32_t frames = buffer_frames_ - padding;
      if (frames == 0)
        continue;

      BYTE* data = nullptr;
      hr = render_client_->GetBuffer(frames, &data);
      if (hr == AUDCLNT_E_DEVICE_INVALIDATED) { device_lost_ = true; SetError("device lost", hr); break; }
      if (FAILED(hr)) continue;

      callback_(user_, scratch_, frames);

      // Stereo scratch -> device layout (float32, channels_ interleaved).
      float* out = reinterpret_cast<float*>(data);
      const float* in = scratch_;
      if (channels_ == 2) {
        for (uint32_t i = 0; i < frames * 2; ++i)
          out[i] = ClampSample(in[i]);
      } else if (channels_ == 1) {
        for (uint32_t i = 0; i < frames; ++i)
          out[i] = ClampSample(0.5f * (in[i * 2] + in[i * 2 + 1]));
      } else {
        for (uint32_t i = 0; i < frames; ++i) {
          float* frame = out + i * channels_;
          frame[0] = ClampSample(in[i * 2]);
          frame[1] = ClampSample(in[i * 2 + 1]);
          for (uint32_t c = 2; c < channels_; ++c)
            frame[c] = 0.0f;
        }
      }
      render_client_->ReleaseBuffer(frames, 0);
    }
  }

  CloseDevice();
  running_ = false;
  if (mmcss != nullptr)
    AvRevertMmThreadCharacteristics(mmcss);
  if (SUCCEEDED(com_hr))
    CoUninitialize();
  return 0;
}

}
}
}
