/*****************************************************************************************************************
* Copyright (c) 2015 Khalid Ali Al-Kooheji                                                                       *
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



namespace dsp {
namespace audio {


class GeneralPlayer {
 public:
  enum State {
    kStateStopped=0,kStatePlaying=1,kStatePaused=2
  };

  GeneralPlayer() : state_(kStateStopped), initialized_(false),audio_interface_(nullptr),
             player_event(nullptr),thread_handle(nullptr),thread_id(0) {
  }
  ~GeneralPlayer() {
    Deinitialize();
  }
  virtual void Initialize();
  virtual void Deinitialize();
  virtual void Play();
  virtual void Pause();
  virtual void Stop();
  virtual void OnPause() = 0;
  virtual void OnPlay() = 0;
  virtual void OnStop() = 0;
  virtual void LoopFunc() = 0;
  void set_audio_interface(output::Interface* audio_interface) { audio_interface_ = (output::Interface*)audio_interface; }
 protected:
  static void __stdcall callback_func(void *parm, float *buf, uint32_t len);
  static DWORD WINAPI PlayThread(LPVOID lpThreadParameter);
  void SendThreadMessage(int msg) {
    EnterCriticalSection(&cs);
    thread_msg = msg;
    LeaveCriticalSection(&cs);
  }
  DWORD InstancePlayThread();
  CRITICAL_SECTION cs;
  audio::output::Interface* audio_interface_;
  double thread_time_span;

  HANDLE thread_handle,player_event;
  DWORD thread_id;
  State state_;
  int thread_msg;
  bool initialized_;

};

}
}